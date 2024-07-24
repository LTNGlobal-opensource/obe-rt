#include "http.h"

#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

char g_stats_http_push_address[128] = { 0 }; // "http://127.0.0.1:13300/nicmonitor/01"

int obe_http_post(const char *msg)
{
	int ret = -1;

	curl_global_init(CURL_GLOBAL_ALL);

	CURL *curl = curl_easy_init();
	if (!curl) {
		return ret;
	}

	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Accept: application/json");
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "charset: utf-8");

	curl_easy_setopt(curl, CURLOPT_URL, g_stats_http_push_address);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, msg);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(msg));
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcrp/0.1");
//      curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
		curl_easy_cleanup(curl);
	} else {
		ret = 0; /* Success */
	}
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	curl_global_cleanup();

	return ret;
}
