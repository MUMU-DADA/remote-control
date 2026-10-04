#pragma once

#include <string>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace remote_control {

inline std::string AndroidProperty(const char* name, const char* fallback = "") {
#if defined(__ANDROID__)
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get(name, value) > 0) return value;
#else
    (void)name;
#endif
    return fallback;
}

inline bool SetAndroidProperty(const char* name, const char* value) {
#if defined(__ANDROID__)
    return __system_property_set(name, value) == 0;
#else
    (void)name;
    (void)value;
    return false;
#endif
}

}  // namespace remote_control
