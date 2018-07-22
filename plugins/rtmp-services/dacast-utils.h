#pragma once 
#include <stdbool.h> 
#include <jansson.h> 
 
 
struct dacast_channel { 
    const char* title; 
    const char* rtmp_endpoint; 
    const char* stream_name; 
    const char* login; 
    const char* password; 
}; 
typedef struct dacast_channel dacast_channel; 
 
 
struct dacast_channel_array { 
    dacast_channel** channels; 
    int count; 
}; 
typedef struct dacast_channel_array dacast_channel_array; 
 
 
struct dacast_result { 
    bool isError; 
    dacast_channel_array* channels_array; 
    const char* error; 
}; 
typedef struct dacast_result dacast_result; 
 
 
extern char* request(const char* url); 
extern dacast_result* get_channels(const char* apikey); 
extern void free_channels(dacast_channel_array* channels); 
extern char* concat(const char *s1, const char *s2); 
extern dacast_channel* channel_from_json_obj(json_t* channel); 
extern dacast_channel* channel_from_json(char* text); 
extern char* channel_to_json(dacast_channel* channel); 
extern dacast_channel* channel_from_json_min(json_t* channel); 
