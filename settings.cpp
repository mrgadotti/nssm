#include "nssm.h"
/* XXX: (value && value->string) is probably bogus because value is probably never null */

extern const TCHAR *exit_action_strings[];
extern const TCHAR *startup_strings[];

/* Does the parameter refer to the default value of the AppExit setting? */
static inline int is_default_exit_action(const TCHAR *value) {
  return (str_equiv(value, _T("default")) || str_equiv(value, _T("*")) || ! value[0]);
}

static int value_from_string(const TCHAR *name, value_t *value, const TCHAR *string) {
  size_t len = _tcslen(string);
  if (! len++) {
    value->string = 0;
    return 0;
  }

  value->string = (TCHAR *) HeapAlloc(GetProcessHeap(), 0, len * sizeof(TCHAR));
  if (! value->string) {
    print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, name, _T("value_from_string()"));
    return -1;
  }

  if (_sntprintf_s(value->string, len, _TRUNCATE, _T("%s"), string) < 0) {
    HeapFree(GetProcessHeap(), 0, value->string);
    print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, name, _T("value_from_string()"));
    return -1;
  }

  return 1;
}

/* Functions to manage NSSM-specific settings in the registry. */
static int setting_set_number(const TCHAR *service_name, void *param, const TCHAR *name, void *default_value, value_t *value, const TCHAR *additional) {
  HKEY key = (HKEY) param;
  if (! key) return -1;

  unsigned long number;
  long error;

  /* Resetting to default? */
  if (! value || ! value->string) {
    error = RegDeleteValue(key, name);
    if (error == ERROR_SUCCESS || error == ERROR_FILE_NOT_FOUND) return 0;
    print_message(stderr, NSSM_MESSAGE_REGDELETEVALUE_FAILED, name, service_name, error_string(error));
    return -1;
  }
  if (str_number(value->string, &number)) return -1;

  if (default_value && number == (unsigned long) default_value) {
    error = RegDeleteValue(key, name);
    if (error == ERROR_SUCCESS || error == ERROR_FILE_NOT_FOUND) return 0;
    print_message(stderr, NSSM_MESSAGE_REGDELETEVALUE_FAILED, name, service_name, error_string(error));
    return -1;
  }

  if (set_number(key, (TCHAR *) name, number)) return -1;

  return 1;
}

static int setting_get_number(const TCHAR *service_name, void *param, const TCHAR *name, void *default_value, value_t *value, const TCHAR *additional) {
  HKEY key = (HKEY) param;
  return get_number(key, (TCHAR *) name, &value->numeric, false);
}

static int setting_set_string(const TCHAR *service_name, void *param, const TCHAR *name, void *default_value, value_t *value, const TCHAR *additional) {
  HKEY key = (HKEY) param;
  if (! key) return -1;

  long error;

  /* Resetting to default? */
  if (! value || ! value->string) {
    if (default_value) value->string = (TCHAR *) default_value;
    else {
      error = RegDeleteValue(key, name);
      if (error == ERROR_SUCCESS || error == ERROR_FILE_NOT_FOUND) return 0;
      print_message(stderr, NSSM_MESSAGE_REGDELETEVALUE_FAILED, name, service_name, error_string(error));
      return -1;
    }
  }
  if (default_value && _tcslen((TCHAR *) default_value) && str_equiv(value->string, (TCHAR *) default_value)) {
    error = RegDeleteValue(key, name);
    if (error == ERROR_SUCCESS || error == ERROR_FILE_NOT_FOUND) return 0;
    print_message(stderr, NSSM_MESSAGE_REGDELETEVALUE_FAILED, name, service_name, error_string(error));
    return -1;
  }

  if (set_expand_string(key, (TCHAR *) name, value->string)) return -1;

  return 1;
}

static int setting_get_string(const TCHAR *service_name, void *param, const TCHAR *name, void *default_value, value_t *value, const TCHAR *additional) {
  HKEY key = (HKEY) param;
  TCHAR buffer[VALUE_LENGTH];

  if (expand_parameter(key, (TCHAR *) name, (TCHAR *) buffer, (unsigned long) sizeof(buffer), false, false)) return -1;

  return value_from_string(name, value, buffer);
}

static int setting_set_exit_action(const TCHAR *service_name, void *param, const TCHAR *name, void *default_value, value_t *value, const TCHAR *additional) {
  unsigned long exitcode;
  TCHAR *code;
  TCHAR action_string[ACTION_LEN];

  if (additional) {
    /* Default action? */
    if (is_default_exit_action(additional)) code = 0;
    else {
      if (str_number(additional, &exitcode)) return -1;
      code = (TCHAR *) additional;
    }
  }

  HKEY key = open_registry(service_name, name, KEY_WRITE);
  if (! key) return -1;

  long error;
  int ret = 1;

  /* Resetting to default? */
  if (value && value->string) _sntprintf_s(action_string, _countof(action_string), _TRUNCATE, _T("%s"), value->string);
  else {
    if (code) {
      /* Delete explicit action. */
      error = RegDeleteValue(key, code);
      RegCloseKey(key);
      if (error == ERROR_SUCCESS || error == ERROR_FILE_NOT_FOUND) return 0;
      print_message(stderr, NSSM_MESSAGE_REGDELETEVALUE_FAILED, code, service_name, error_string(error));
      return -1;
    }
    else {
      /* Explicitly keep the default action. */
      if (default_value) _sntprintf_s(action_string, _countof(action_string), _TRUNCATE, _T("%s"), (TCHAR *) default_value);
      ret = 0;
    }
  }

  /* Validate the string. */
  for (int i = 0; exit_action_strings[i]; i++) {
    if (! _tcsnicmp((const TCHAR *) action_string, exit_action_strings[i], ACTION_LEN)) {
      if (default_value && str_equiv(action_string, (TCHAR *) default_value)) ret = 0;
      if (RegSetValueEx(key, code, 0, REG_SZ, (const unsigned char *) action_string, (unsigned long) (_tcslen(action_string) + 1) * sizeof(TCHAR)) != ERROR_SUCCESS) {
        print_message(stderr, NSSM_MESSAGE_REGDELETEVALUE_FAILED, code, service_name, error_string(GetLastError()));
        RegCloseKey(key);
        return -1;
      }

      RegCloseKey(key);
      return ret;
    }
  }

  print_message(stderr, NSSM_MESSAGE_INVALID_EXIT_ACTION, action_string);
  for (int i = 0; exit_action_strings[i]; i++) _ftprintf(stderr, _T("%s\n"), exit_action_strings[i]);

  return -1;
}

static int setting_get_exit_action(const TCHAR *service_name, void *param, const TCHAR *name, void *default_value, value_t *value, const TCHAR *additional) {
  unsigned long exitcode = 0;
  unsigned long *code = 0;

  if (additional) {
    if (! is_default_exit_action(additional)) {
      if (str_number(additional, &exitcode)) return -1;
      code = &exitcode;
    }
  }

  TCHAR action_string[ACTION_LEN];
  bool default_action;
  if (get_exit_action(service_name, code, action_string, &default_action)) return -1;

  value_from_string(name, value, action_string);

  if (default_action && ! _tcsnicmp((const TCHAR *) action_string, (TCHAR *) default_value, ACTION_LEN)) return 0;
  return 1;
}

static int setting_set_environment(const TCHAR *service_name, void *param, const TCHAR *name, void *default_value, value_t *value, const TCHAR *additional) {
  HKEY key = (HKEY) param;
  if (! param) return -1;

  if (! value || ! value->string || ! value->string[0]) {
    long error = RegDeleteValue(key, name);
    if (error == ERROR_SUCCESS || error == ERROR_FILE_NOT_FOUND) return 0;
    print_message(stderr, NSSM_MESSAGE_REGDELETEVALUE_FAILED, name, service_name, error_string(error));
    return -1;
  }

  unsigned long envlen = (unsigned long) _tcslen(value->string) + 1;
  TCHAR *unformatted = 0;
  unsigned long newlen;
  if (unformat_environment(value->string, envlen, &unformatted, &newlen)) return -1;

  if (test_environment(unformatted)) {
    HeapFree(GetProcessHeap(), 0, unformatted);
    print_message(stderr, NSSM_GUI_INVALID_ENVIRONMENT);
    return -1;
  }

  if (RegSetValueEx(key, name, 0, REG_MULTI_SZ, (const unsigned char *) unformatted, (unsigned long) newlen * sizeof(TCHAR)) != ERROR_SUCCESS) {
    if (newlen) HeapFree(GetProcessHeap(), 0, unformatted);
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_SETVALUE_FAILED, NSSM_REG_ENV, error_string(GetLastError()), 0);
    return -1;
  }

  if (newlen) HeapFree(GetProcessHeap(), 0, unformatted);
  return 1;
}

static int setting_get_environment(const TCHAR *service_name, void *param, const TCHAR *name, void *default_value, value_t *value, const TCHAR *additional) {
  HKEY key = (HKEY) param;
  if (! param) return -1;

  TCHAR *env = 0;
  unsigned long envlen;
  if (set_environment((TCHAR *) service_name, key, (TCHAR *) name, &env, &envlen)) return -1;
  if (! envlen) return 0;

  TCHAR *formatted;
  unsigned long newlen;
  if (format_environment(env, envlen, &formatted, &newlen)) return -1;

  int ret;
  if (additional) {
    /* Find named environment variable. */
    TCHAR *s;
    size_t len = _tcslen(additional);
    for (s = env; *s; s++) {
      /* Look for <additional>=<string> NULL NULL */
      if (! _tcsnicmp(s, additional, len) && s[len] == _T('=')) {
        /* Strip <key>= */
        s += len + 1;
        ret = value_from_string(name, value, s);
        HeapFree(GetProcessHeap(), 0, env);
        return ret;
      }

      /* Skip this string. */
      for ( ; *s; s++);
    }
    HeapFree(GetProcessHeap(), 0, env);
    return 0;
  }

  HeapFree(GetProcessHeap(), 0, env);

  ret = value_from_string(name, value, formatted);
  if (newlen) HeapFree(GetProcessHeap(), 0, formatted);
  return ret;
}

/* Functions to manage native service settings. */
int native_set_description(const TCHAR *service_name, void *param, const TCHAR *name, void *default_value, value_t *value, const TCHAR *additional) {
  SC_HANDLE service_handle = (SC_HANDLE) param;
  if (! service_handle) return -1;

  TCHAR *description = 0;
  if (value) description = value->string;
  if (set_service_description(service_name, service_handle, description)) return -1;

  if (description && description[0]) return 1;

  return 0;
}

int native_get_description(const TCHAR *service_name, void *param, const TCHAR *name, void *default_value, value_t *value, const TCHAR *additional) {
  SC_HANDLE service_handle = (SC_HANDLE) param;
  if (! service_handle) return -1;

  TCHAR buffer[VALUE_LENGTH];
  if (get_service_description(service_name, service_handle, _countof(buffer), buffer)) return -1;

  if (buffer[0]) return value_from_string(name, value, buffer);
  value->string = 0;

  return 0;
}

int native_set_displayname(const TCHAR *service_name, void *param, const TCHAR *name, void *default_value, value_t *value, const TCHAR *additional) {
  SC_HANDLE service_handle = (SC_HANDLE) param;
  if (! service_handle) return -1;

  TCHAR *displayname = 0;
  if (value && value->string) displayname = value->string;
  else displayname = (TCHAR *) service_name;

  if (! ChangeServiceConfig(service_handle, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, 0, 0, 0, 0, 0, 0, displayname)) {
    print_message(stderr, NSSM_MESSAGE_CHANGESERVICECONFIG_FAILED, error_string(GetLastError()));
    return -1;
  }

  /*
    If the display name and service name differ only in case,
    ChangeServiceConfig() will return success but the display name will be
    set to the service name, NOT the value passed to the function.
    This appears to be a quirk of Windows rather than a bug here.
  */
  if (displayname != service_name && ! str_equiv(displayname, service_name)) return 1;

  return 0;
}

int native_get_displayname(const TCHAR *service_name, void *param, const TCHAR *name, void *default_value, value_t *value, const TCHAR *additional) {
  SC_HANDLE service_handle = (SC_HANDLE) param;
  if (! service_handle) return -1;

  QUERY_SERVICE_CONFIG *qsc = query_service_config(service_name, service_handle);
  if (! qsc) return -1;

  int ret = value_from_string(name, value, qsc->lpDisplayName);
  HeapFree(GetProcessHeap(), 0, qsc);

  return ret;
}

int native_set_imagepath(const TCHAR *service_name, void *param, const TCHAR *name, void *default_value, value_t *value, const TCHAR *additional) {
  SC_HANDLE service_handle = (SC_HANDLE) param;
  if (! service_handle) return -1;

  /* It makes no sense to try to reset the image path. */
  if (! value || ! value->string) {
    print_message(stderr, NSSM_MESSAGE_NO_DEFAULT_VALUE, name);
    return -1;
  }

  if (! ChangeServiceConfig(service_handle, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, value->string, 0, 0, 0, 0, 0, 0)) {
    print_message(stderr, NSSM_MESSAGE_CHANGESERVICECONFIG_FAILED, error_string(GetLastError()));
    return -1;
  }

  return 1;
}

int native_get_imagepath(const TCHAR *service_name, void *param, const TCHAR *name, void *default_value, value_t *value, const TCHAR *additional) {
  SC_HANDLE service_handle = (SC_HANDLE) param;
  if (! service_handle) return -1;

  QUERY_SERVICE_CONFIG *qsc = query_service_config(service_name, service_handle);
  if (! qsc) return -1;

  int ret = value_from_string(name, value, qsc->lpBinaryPathName);
  HeapFree(GetProcessHeap(), 0, qsc);

  return ret;
}

int native_set_objectname(const TCHAR *service_name, void *param, const TCHAR *name, void *default_value, value_t *value, const TCHAR *additional) {
  SC_HANDLE service_handle = (SC_HANDLE) param;
  if (! service_handle) return -1;

  /*
    Logical syntax is: nssm set <service> ObjectName <username> <password>
    That means the username is actually passed in the additional parameter.
  */
  bool localsystem = true;
  TCHAR *username = NSSM_LOCALSYSTEM_ACCOUNT;
  TCHAR *password = 0;
  if (additional) {
    if (! str_equiv(additional, NSSM_LOCALSYSTEM_ACCOUNT)) {
      localsystem = false;
      username = (TCHAR *) additional;
      if (value && value->string) password = value->string;
      else {
        /* We need a password if the account is not LOCALSYSTEM. */
        print_message(stderr, NSSM_MESSAGE_MISSING_PASSWORD, name);
        return -1;
      }
    }
  }

  /*
    ChangeServiceConfig() will fail to set the username if the service is set
    to interact with the desktop.
  */
  unsigned long type = SERVICE_NO_CHANGE;
  if (! localsystem) {
    QUERY_SERVICE_CONFIG *qsc = query_service_config(service_name, service_handle);
    if (! qsc) {
      if (password) SecureZeroMemory(password, _tcslen(password) * sizeof(TCHAR));
      return -1;
    }

    type = qsc->dwServiceType & ~SERVICE_INTERACTIVE_PROCESS;
    HeapFree(GetProcessHeap(), 0, qsc);
  }

  if (grant_logon_as_service(username)) {
    if (password) SecureZeroMemory(password, _tcslen(password) * sizeof(TCHAR));
    print_message(stderr, NSSM_MESSAGE_GRANT_LOGON_AS_SERVICE_FAILED, username);
    return -1;
  }

  if (! ChangeServiceConfig(service_handle, type, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, 0, 0, 0, 0, username, password, 0)) {
    if (password) SecureZeroMemory(password, _tcslen(password) * sizeof(TCHAR));
    print_message(stderr, NSSM_MESSAGE_CHANGESERVICECONFIG_FAILED, error_string(GetLastError()));
    return -1;
  }
  if (password) SecureZeroMemory(password, _tcslen(password) * sizeof(TCHAR));

  if (localsystem) return 0;

  return 1;
}

int native_get_objectname(const TCHAR *service_name, void *param, const TCHAR *name, void *default_value, value_t *value, const TCHAR *additional) {
  SC_HANDLE service_handle = (SC_HANDLE) param;
  if (! service_handle) return -1;

  QUERY_SERVICE_CONFIG *qsc = query_service_config(service_name, service_handle);
  if (! qsc) return -1;

  int ret = value_from_string(name, value, qsc->lpServiceStartName);
  HeapFree(GetProcessHeap(), 0, qsc);

  return ret;
}

int native_set_startup(const TCHAR *service_name, void *param, const TCHAR *name, void *default_value, value_t *value, const TCHAR *additional) {
  SC_HANDLE service_handle = (SC_HANDLE) param;
  if (! service_handle) return -1;

  /* It makes no sense to try to reset the startup type. */
  if (! value || ! value->string) {
    print_message(stderr, NSSM_MESSAGE_NO_DEFAULT_VALUE, name);
    return -1;
  }

  /* Map NSSM_STARTUP_* constant to Windows SERVICE_*_START constant. */
  int service_startup = -1;
  int i;
  for (i = 0; startup_strings[i]; i++) {
    if (str_equiv(value->string, startup_strings[i])) {
      service_startup = i;
      break;
    }
  }

  if (service_startup < 0) {
    print_message(stderr, NSSM_MESSAGE_INVALID_SERVICE_STARTUP, value->string);
    for (i = 0; startup_strings[i]; i++) _ftprintf(stderr, _T("%s\n"), startup_strings[i]);
    return -1;
  }

  unsigned long startup;
  switch (service_startup) {
    case NSSM_STARTUP_MANUAL: startup = SERVICE_DEMAND_START; break;
    case NSSM_STARTUP_DISABLED: startup = SERVICE_DISABLED; break;
    default: startup = SERVICE_AUTO_START;
  }

  if (! ChangeServiceConfig(service_handle, SERVICE_NO_CHANGE, startup, SERVICE_NO_CHANGE, 0, 0, 0, 0, 0, 0, 0)) {
    print_message(stderr, NSSM_MESSAGE_CHANGESERVICECONFIG_FAILED, error_string(GetLastError()));
    return -1;
  }

  SERVICE_DELAYED_AUTO_START_INFO delayed;
  ZeroMemory(&delayed, sizeof(delayed));
  if (service_startup == NSSM_STARTUP_DELAYED) delayed.fDelayedAutostart = 1;
  else delayed.fDelayedAutostart = 0;
  if (! ChangeServiceConfig2(service_handle, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed)) {
    unsigned long error = GetLastError();
    /* Pre-Vista we expect to fail with ERROR_INVALID_LEVEL */
    if (error != ERROR_INVALID_LEVEL) {
      log_event(EVENTLOG_ERROR_TYPE, NSSM_MESSAGE_SERVICE_CONFIG_DELAYED_AUTO_START_INFO_FAILED, service_name, error_string(error), 0);
    }
  }

  return 1;
}

int native_get_startup(const TCHAR *service_name, void *param, const TCHAR *name, void *default_value, value_t *value, const TCHAR *additional) {
  SC_HANDLE service_handle = (SC_HANDLE) param;
  if (! service_handle) return -1;

  QUERY_SERVICE_CONFIG *qsc = query_service_config(service_name, service_handle);
  if (! qsc) return -1;

  unsigned long startup;
  int ret = get_service_startup(service_name, service_handle, qsc, &startup);
  HeapFree(GetProcessHeap(), 0, qsc);

  if (ret) return -1;

  unsigned long i;
  for (i = 0; startup_strings[i]; i++);
  if (startup >= i) return -1;

  return value_from_string(name, value, startup_strings[startup]);
}

int native_set_type(const TCHAR *service_name, void *param, const TCHAR *name, void *default_value, value_t *value, const TCHAR *additional) {
  SC_HANDLE service_handle = (SC_HANDLE) param;
  if (! service_handle) return -1;

  /* It makes no sense to try to reset the service type. */
  if (! value || ! value->string) {
    print_message(stderr, NSSM_MESSAGE_NO_DEFAULT_VALUE, name);
    return -1;
  }

  /*
    We can only manage services of type SERVICE_WIN32_OWN_PROCESS
    and SERVICE_INTERACTIVE_PROCESS.
  */
  unsigned long type = SERVICE_WIN32_OWN_PROCESS;
  if (str_equiv(value->string, NSSM_INTERACTIVE_PROCESS)) type |= SERVICE_INTERACTIVE_PROCESS;
  else if (! str_equiv(value->string, NSSM_WIN32_OWN_PROCESS)) {
    print_message(stderr, NSSM_MESSAGE_INVALID_SERVICE_TYPE, value->string);
    _ftprintf(stderr, _T("%s\n"), NSSM_WIN32_OWN_PROCESS);
    _ftprintf(stderr, _T("%s\n"), NSSM_INTERACTIVE_PROCESS);
    return -1;
  }

  /*
    ChangeServiceConfig() will fail if the service runs under an account
    other than LOCALSYSTEM and we try to make it interactive.
  */
  if (type & SERVICE_INTERACTIVE_PROCESS) {
    QUERY_SERVICE_CONFIG *qsc = query_service_config(service_name, service_handle);
    if (! qsc) return -1;

    if (! str_equiv(qsc->lpServiceStartName, NSSM_LOCALSYSTEM_ACCOUNT)) {
      HeapFree(GetProcessHeap(), 0, qsc);
      print_message(stderr, NSSM_MESSAGE_INTERACTIVE_NOT_LOCALSYSTEM, value->string, service_name, NSSM_LOCALSYSTEM_ACCOUNT);
      return -1;
    }

    HeapFree(GetProcessHeap(), 0, qsc);
  }

  if (! ChangeServiceConfig(service_handle, type, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, 0, 0, 0, 0, 0, 0, 0)) {
    print_message(stderr, NSSM_MESSAGE_CHANGESERVICECONFIG_FAILED, error_string(GetLastError()));
    return -1;
  }

  return 1;
}

int native_get_type(const TCHAR *service_name, void *param, const TCHAR *name, void *default_value, value_t *value, const TCHAR *additional) {
  SC_HANDLE service_handle = (SC_HANDLE) param;
  if (! service_handle) return -1;

  QUERY_SERVICE_CONFIG *qsc = query_service_config(service_name, service_handle);
  if (! qsc) return -1;

  value->numeric = qsc->dwServiceType;
  HeapFree(GetProcessHeap(), 0, qsc);

  const TCHAR *string;
  switch (value->numeric) {
    case SERVICE_KERNEL_DRIVER: string = NSSM_KERNEL_DRIVER; break;
    case SERVICE_FILE_SYSTEM_DRIVER: string = NSSM_FILE_SYSTEM_DRIVER; break;
    case SERVICE_WIN32_OWN_PROCESS: string = NSSM_WIN32_OWN_PROCESS; break;
    case SERVICE_WIN32_SHARE_PROCESS: string = NSSM_WIN32_SHARE_PROCESS; break;
    case SERVICE_WIN32_OWN_PROCESS|SERVICE_INTERACTIVE_PROCESS: string = NSSM_INTERACTIVE_PROCESS; break;
    case SERVICE_WIN32_SHARE_PROCESS|SERVICE_INTERACTIVE_PROCESS: string = NSSM_SHARE_INTERACTIVE_PROCESS; break;
    default: string = NSSM_UNKNOWN;
  }

  return value_from_string(name, value, string);
}

int set_setting(const TCHAR *service_name, HKEY key, settings_t *setting, value_t *value, const TCHAR *additional) {
  if (! key) return -1;
  int ret;

  if (setting->set) ret = setting->set(service_name, (void *) key, setting->name, setting->default_value, value, additional);
  else ret = -1;

  if (! ret) print_message(stdout, NSSM_MESSAGE_RESET_SETTING, setting->name, service_name);
  else if (ret > 0) print_message(stdout, NSSM_MESSAGE_SET_SETTING, setting->name, service_name);
  else print_message(stderr, NSSM_MESSAGE_SET_SETTING_FAILED, setting->name, service_name);

  return ret;
}

int set_setting(const TCHAR *service_name, SC_HANDLE service_handle, settings_t *setting, value_t *value, const TCHAR *additional) {
  if (! service_handle) return -1;

  int ret;
  if (setting->set) ret = setting->set(service_name, service_handle, setting->name, setting->default_value, value, additional);
  else ret = -1;

  if (! ret) print_message(stdout, NSSM_MESSAGE_RESET_SETTING, setting->name, service_name);
  else if (ret > 0) print_message(stdout, NSSM_MESSAGE_SET_SETTING, setting->name, service_name);
  else print_message(stderr, NSSM_MESSAGE_SET_SETTING_FAILED, setting->name, service_name);

  return ret;
}

/*
  Returns:  1 if the value was retrieved.
            0 if the default value was retrieved.
           -1 on error.
*/
int get_setting(const TCHAR *service_name, HKEY key, settings_t *setting, value_t *value, const TCHAR *additional) {
  if (! key) return -1;
  int ret;

  switch (setting->type) {
    case REG_EXPAND_SZ:
    case REG_MULTI_SZ:
    case REG_SZ:
      value->string = (TCHAR *) setting->default_value;
      if (setting->get) ret = setting->get(service_name, (void *) key, setting->name, setting->default_value, value, additional);
      else ret = -1;
      break;

    case REG_DWORD:
      value->numeric = (unsigned long) setting->default_value;
      if (setting->get) ret = setting->get(service_name, (void *) key, setting->name, setting->default_value, value, additional);
      else ret = -1;
      break;

    default:
      ret = -1;
      break;
  }

  if (ret < 0) print_message(stderr, NSSM_MESSAGE_GET_SETTING_FAILED, setting->name, service_name);

  return ret;
}

int get_setting(const TCHAR *service_name, SC_HANDLE service_handle, settings_t *setting, value_t *value, const TCHAR *additional) {
  if (! service_handle) return -1;
  return setting->get(service_name, service_handle, setting->name, 0, value, additional);
}

settings_t settings[] = {
  { NSSM_REG_EXE, REG_EXPAND_SZ, (void *) _T(""), false, 0, setting_set_string, setting_get_string },
  { NSSM_REG_FLAGS, REG_EXPAND_SZ, (void *) _T(""), false, 0, setting_set_string, setting_get_string },
  { NSSM_REG_DIR, REG_EXPAND_SZ, (void *) _T(""), false, 0, setting_set_string, setting_get_string },
  { NSSM_REG_EXIT, REG_SZ, (void *) exit_action_strings[NSSM_EXIT_RESTART], false, ADDITIONAL_MANDATORY, setting_set_exit_action, setting_get_exit_action },
  { NSSM_REG_ENV, REG_MULTI_SZ, NULL, false, ADDITIONAL_CRLF, setting_set_environment, setting_get_environment },
  { NSSM_REG_ENV_EXTRA, REG_MULTI_SZ, NULL, false, ADDITIONAL_CRLF, setting_set_environment, setting_get_environment },
  { NSSM_REG_STDIN, REG_EXPAND_SZ, NULL, false, 0, setting_set_string, setting_get_string },
  { NSSM_REG_STDIN NSSM_REG_STDIO_SHARING, REG_DWORD, (void *) NSSM_STDIN_SHARING, false, 0, setting_set_number, setting_get_number },
  { NSSM_REG_STDIN NSSM_REG_STDIO_DISPOSITION, REG_DWORD, (void *) NSSM_STDIN_DISPOSITION, false, 0, setting_set_number, setting_get_number },
  { NSSM_REG_STDIN NSSM_REG_STDIO_FLAGS, REG_DWORD, (void *) NSSM_STDIN_FLAGS, false, 0, setting_set_number, setting_get_number },
  { NSSM_REG_STDOUT, REG_EXPAND_SZ, NULL, false, 0, setting_set_string, setting_get_string },
  { NSSM_REG_STDOUT NSSM_REG_STDIO_SHARING, REG_DWORD, (void *) NSSM_STDOUT_SHARING, false, 0, setting_set_number, setting_get_number },
  { NSSM_REG_STDOUT NSSM_REG_STDIO_DISPOSITION, REG_DWORD, (void *) NSSM_STDOUT_DISPOSITION, false, 0, setting_set_number, setting_get_number },
  { NSSM_REG_STDOUT NSSM_REG_STDIO_FLAGS, REG_DWORD, (void *) NSSM_STDOUT_FLAGS, false, 0, setting_set_number, setting_get_number },
  { NSSM_REG_STDERR, REG_EXPAND_SZ, NULL, false, 0, setting_set_string, setting_get_string },
  { NSSM_REG_STDERR NSSM_REG_STDIO_SHARING, REG_DWORD, (void *) NSSM_STDERR_SHARING, false, 0, setting_set_number, setting_get_number },
  { NSSM_REG_STDERR NSSM_REG_STDIO_DISPOSITION, REG_DWORD, (void *) NSSM_STDERR_DISPOSITION, false, 0, setting_set_number, setting_get_number },
  { NSSM_REG_STDERR NSSM_REG_STDIO_FLAGS, REG_DWORD, (void *) NSSM_STDERR_FLAGS, false, 0, setting_set_number, setting_get_number },
  { NSSM_REG_STOP_METHOD_SKIP, REG_DWORD, 0, false, 0, setting_set_number, setting_get_number },
  { NSSM_REG_KILL_CONSOLE_GRACE_PERIOD, REG_DWORD, (void *) NSSM_KILL_CONSOLE_GRACE_PERIOD, false, 0, setting_set_number, setting_get_number },
  { NSSM_REG_KILL_WINDOW_GRACE_PERIOD, REG_DWORD, (void *) NSSM_KILL_WINDOW_GRACE_PERIOD, false, 0, setting_set_number, setting_get_number },
  { NSSM_REG_KILL_THREADS_GRACE_PERIOD, REG_DWORD, (void *) NSSM_KILL_THREADS_GRACE_PERIOD, false, 0, setting_set_number, setting_get_number },
  { NSSM_REG_THROTTLE, REG_DWORD, (void *) NSSM_RESET_THROTTLE_RESTART, false, 0, setting_set_number, setting_get_number },
  { NSSM_REG_ROTATE, REG_DWORD, 0, false, 0, setting_set_number, setting_get_number },
  { NSSM_REG_ROTATE_SECONDS, REG_DWORD, 0, false, 0, setting_set_number, setting_get_number },
  { NSSM_REG_ROTATE_BYTES_LOW, REG_DWORD, 0, false, 0, setting_set_number, setting_get_number },
  { NSSM_REG_ROTATE_BYTES_HIGH, REG_DWORD, 0, false, 0, setting_set_number, setting_get_number },
  { NSSM_NATIVE_DESCRIPTION, REG_SZ, _T(""), true, 0, native_set_description, native_get_description },
  { NSSM_NATIVE_DISPLAYNAME, REG_SZ, NULL, true, 0, native_set_displayname, native_get_displayname },
  { NSSM_NATIVE_IMAGEPATH, REG_EXPAND_SZ, NULL, true, 0, native_set_imagepath, native_get_imagepath },
  { NSSM_NATIVE_OBJECTNAME, REG_SZ, NSSM_LOCALSYSTEM_ACCOUNT, true, ADDITIONAL_SETTING, native_set_objectname, native_get_objectname },
  { NSSM_NATIVE_STARTUP, REG_SZ, NULL, true, 0, native_set_startup, native_get_startup },
  { NSSM_NATIVE_TYPE, REG_SZ, NULL, true, 0, native_set_type, native_get_type },
  { NULL, NULL, NULL, NULL, NULL }
};