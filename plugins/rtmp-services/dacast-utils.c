#include <curl/curl.h> 
#include <jansson.h> 
#include <string.h> 
#include <obs-module.h> 
#include "dacast-utils.h" 
 
#define BUFFER_SIZE  (256 * 1024)  /* 256 KB */ 
 
struct write_result 
{ 
    char *data; 
    int pos; 
}; 
 
static size_t write_response(void *ptr, size_t size, size_t nmemb, void *stream) 
{ 
    struct write_result *result = (struct write_result *)stream; 
 
    if(result->pos + size * nmemb >= BUFFER_SIZE - 1) 
    { 
        fprintf(stderr, "error: too small buffer\n"); 
        return 0; 
    } 
 
    memcpy(result->data + result->pos, ptr, size * nmemb); 
    result->pos += size * nmemb; 
 
    return size * nmemb; 
} 
 
 
char *request(const char *url) 
{ 
    CURL *curl = NULL; 
    CURLcode status; 
    struct curl_slist *headers = NULL; 
    char *data = NULL; 
    long code; 
 
    curl_global_init(CURL_GLOBAL_ALL); 
    curl = curl_easy_init(); 
    if(!curl) 
        goto error; 
 
    data = malloc(BUFFER_SIZE); 
    if(!data) 
        goto error; 
 
    struct write_result write_result = { 
        .data = data, 
        .pos = 0 
    }; 
 
    curl_easy_setopt(curl, CURLOPT_URL, url); 
 
    /* GitHub commits API v3 requires a User-Agent header */ 
    headers = curl_slist_append(headers, "User-Agent: DaCast-OBS"); 
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers); 
 
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response); 
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_result); 
 
    status = curl_easy_perform(curl); 
    if(status != 0) 
    { 
        fprintf(stderr, "error: unable to request data from %s:\n", url); 
        fprintf(stderr, "%s\n", curl_easy_strerror(status)); 
        goto error; 
    } 
 
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code); 
    if(code != 200) 
    { 
        fprintf(stderr, "error: server responded with code %ld\n", code); 
        goto error; 
    } 
 
    curl_easy_cleanup(curl); 
    curl_slist_free_all(headers); 
    curl_global_cleanup(); 
 
    /* zero-terminate the result */ 
    data[write_result.pos] = '\0'; 
 
    return data; 
 
error: 
    if(data) 
        free(data); 
    if(curl) 
        curl_easy_cleanup(curl); 
    if(headers) 
        curl_slist_free_all(headers); 
    curl_global_cleanup(); 
    return NULL; 
} 
 
char* concat(const char *s1, const char *s2) 
{ 
    char *result = bzalloc(strlen(s1) + strlen(s2) + 1);//+1 for the null-terminator 
    //in real code you would check for errors in malloc here 
    strncpy(result, s1, strlen(s1)); 
    strcat(result, s2); 
    return result; 
} 
 
char* channel_to_json(dacast_channel* channel) 
{ 
    json_t* json; 
    char* result; 
 
    json = json_object(); 
    json_object_set_new(json, "title", json_string(bstrdup(channel->title))); 
    json_object_set_new(json, "rtmp_endpoint", json_string(bstrdup(channel->rtmp_endpoint))); 
    json_object_set_new(json, "stream_name", json_string(bstrdup(channel->stream_name))); 
    json_object_set_new(json, "login", json_string(bstrdup(channel->login))); 
    json_object_set_new(json, "password", json_string(bstrdup(channel->password))); 
     
    result = json_dumps(json, 0); 
    json_decref(json); 
 
    return result; 
} 
 
dacast_channel* channel_from_json(char* text) 
{ 
    json_t* channel; 
    json_error_t error; 
    dacast_channel* result; 
 
    channel = json_loads(text, 0, &error); 
    if(!channel) 
    { 
        blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>error reading json: %s", error.text); 
        return NULL; 
    } 
 
    result = channel_from_json_min(channel); 
    json_decref(channel); 
    return result; 
} 
 
dacast_channel* channel_from_json_min(json_t* channel) 
{ 
    json_t *title_json, *rtmp_endpoint_json, *stream_name_json, *login_json, *password_json; 
    const char *title, *rtmp_endpoint, *stream_name, *login, *password; 
    dacast_channel* dc_channel; 
     
    title_json = json_object_get(channel, "title"); 
    if(!json_is_string(title_json)) 
        return NULL; 
    title = json_string_value(title_json); 
 
    rtmp_endpoint_json = json_object_get(channel, "rtmp_endpoint"); 
    if(!json_is_string(rtmp_endpoint_json)) 
        return NULL; 
    rtmp_endpoint = json_string_value(rtmp_endpoint_json); 
 
    stream_name_json = json_object_get(channel, "stream_name"); 
    if(!json_is_string(stream_name_json)) 
        return NULL; 
    stream_name = json_string_value(stream_name_json); 
 
    login_json = json_object_get(channel, "login"); 
    if(!json_is_string(login_json)) 
        return NULL; 
    login = json_string_value(login_json); 
 
    password_json = json_object_get(channel, "password"); 
    if(!json_is_string(password_json)) 
        return NULL; 
    password = json_string_value(password_json); 
 
    dc_channel = bzalloc(sizeof(dacast_channel)); 
    dc_channel->title           = bstrdup(title); 
    dc_channel->rtmp_endpoint   = bstrdup(rtmp_endpoint); 
    dc_channel->stream_name     = bstrdup(stream_name); 
    dc_channel->login           = bstrdup(login); 
    dc_channel->password        = bstrdup(password); 
    return dc_channel; 
} 
 
dacast_channel* channel_from_json_obj(json_t* channel) 
{ 
    json_t *config; 
    json_t *title_json, *rtmp_endpoint_json, *stream_name_json, *login_json, *password_json; 
    const char *title, *rtmp_endpoint, *stream_name, *login, *password; 
    dacast_channel* dc_channel; 
 
    config = json_object_get(channel, "config"); 
    if(!json_is_object(config)) 
        return NULL; 
 
 
    title_json = json_object_get(channel, "title"); 
    if(!json_is_string(title_json)) 
        return NULL; 
    title = json_string_value(title_json); 
 
    rtmp_endpoint_json = json_object_get(config, "publishing_point_primary"); 
    if(!json_is_string(rtmp_endpoint_json)) 
        return NULL; 
    rtmp_endpoint = json_string_value(rtmp_endpoint_json); 
 
    stream_name_json = json_object_get(config, "stream_name"); 
    if(!json_is_string(stream_name_json)) 
        return NULL; 
    stream_name = json_string_value(stream_name_json); 
 
    login_json = json_object_get(config, "login"); 
    if(!json_is_string(login_json)) 
        return NULL; 
    login = json_string_value(login_json); 
 
    password_json = json_object_get(config, "password"); 
    if(!json_is_string(password_json)) 
        return NULL; 
    password = json_string_value(password_json); 
 
    dc_channel = bzalloc(sizeof(dacast_channel)); 
    dc_channel->title           = bstrdup(title); 
    dc_channel->rtmp_endpoint   = bstrdup(rtmp_endpoint); 
    dc_channel->stream_name     = bstrdup(stream_name); 
    dc_channel->login           = bstrdup(login); 
    dc_channel->password        = bstrdup(password); 
    return dc_channel; 
} 
 
dacast_result* get_channels(const char* apikey) 
{ 
    char* text; 
    const char* base_url = "https://api.dacast.com/v2/channel?apikey="; 
    char* url = concat(base_url, apikey); 
     
    size_t i; 
    size_t data_length; 
    json_t* root; 
    json_t* data; 
    json_error_t error; 
    dacast_channel_array* channels; 
    dacast_result* result = bzalloc(sizeof(dacast_result)); 
 
    text = request(url); 
    bfree(url); 
 
    if(!text) 
    { 
        result->isError = true; 
        result->error = "Received empty response"; 
        return result; 
    } 
     
    root = json_loads(text, 0, &error); 
    bfree(text); 
 
    if(!root) 
    { 
        result->isError = true; 
        result->error = "Received malformated JSON"; 
        return result; 
    } 
 
    if(!json_is_object(root) || !json_is_array(json_object_get(root, "data"))) 
    { 
        result->isError = true; 
        result->error = "The received JSON structure is unexpected"; 
        json_decref(root); 
        return result; 
    } 
 
    data = json_object_get(root, "data"); 
    data_length = json_array_size(data); 
    channels = bzalloc(sizeof(dacast_channel_array)); 
    channels->channels = bzalloc(sizeof(dacast_channel*) * data_length); 
    channels->count = data_length; 
    for(i = 0; i < data_length; i++) 
    { 
        json_t *channel; 
        dacast_channel* dc_channel; 
 
 
        channel = json_array_get(data, i); 
        if(!json_is_object(channel)) 
        { 
            result->isError = true; 
            result->error = "One of the data member received is not an object"; 
            json_decref(root); 
            free_channels(channels); 
            return result; 
        } 
         
        dc_channel = channel_from_json_obj(channel); 
        if(dc_channel == NULL) 
        { 
            result->isError = true; 
            result->error = "One of the members of the channel is not correctly formatted"; 
            json_decref(root); 
            free_channels(channels); 
            return result; 
        } 
 
        channels->channels[i] = dc_channel; 
    } 
 
    json_decref(root); 
    result->isError = false; 
    result->channels_array = channels; 
    return result; 
} 
//TODO free dacast_result 
//TODO free all the channels one by one instead of free the pinters only 
//TODO use obs malloc/strdup and free methods instead of the C ones 
 
void free_channels(dacast_channel_array* channels) 
{ 
    if(channels == NULL) 
        return; 
 
    int i; 
    for(i = 0; i < channels->count; i++) 
    { 
        dacast_channel* channel = channels->channels[i]; 
        bfree(channel); 
    } 
    bfree(channels); 
}