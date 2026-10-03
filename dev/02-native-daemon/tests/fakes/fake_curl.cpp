#include <cstdarg>

extern "C" {

void* curl_easy_init() { return nullptr; }
void curl_easy_cleanup(void*) {}
int curl_easy_setopt(void*, int, ...) { return 0; }
int curl_easy_perform(void*) { return 0; }
const char* curl_version() { return "fake-curl-for-lifecycle-test"; }

}
