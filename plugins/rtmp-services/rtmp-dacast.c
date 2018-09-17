#include <obs-module.h> 
#include "dacast-utils.h" 
//TODO use obs_module_text to have translation of parameter names 
 
 
struct rtmp_dacast { 
    const char* key; 
    const char* url; 
    const char* username; 
    const char* password; 
}; 
typedef struct rtmp_dacast rtmp_dacast; 
 
static char* apikey_static; 
 
static const char* dacast_get_name(void* unused) 
{ 
    UNUSED_PARAMETER(unused); 
    return "Dacast"; 
} 
 
static void dacast_update(void* data, obs_data_t* settings) 
{ 
    rtmp_dacast* service = data; 
 
    // blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>update called"); 
 
    apikey_static = bstrdup(obs_data_get_string(settings, "apikey")); 
    char* channel_json = bstrdup(obs_data_get_string(settings, "channel_list")); 
    dacast_channel* channel = channel_from_json(channel_json); 
    bfree(channel_json); 
    if(channel == NULL) 
    { 
        // blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>not updating because got null channel from json %s", channel_json); 
        return; 
    } 
 
    service->url = bstrdup(channel->rtmp_endpoint); 
    service->key = bstrdup(channel->stream_name); 
    service->username = bstrdup(channel->login); 
    service->password = bstrdup(channel->password); 
 
    bfree(channel); 
 
    // blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>update url:%s", service->url); 
    // blog(LOG_INFO, "update key:%s", service->key); 
    // blog(LOG_INFO, "update username:%s", service->username); 
    // blog(LOG_INFO, "update password:%s", service->password); 
     
} 
 
static void dacast_destroy(void* data){ 
    rtmp_dacast* service = data; 
 
    // blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>DESTROY CALLED"); 
 
    bfree(apikey_static); 
    apikey_static = NULL; 
    bfree(service->url); 
    bfree(service->key); 
    bfree(service->username); 
    bfree(service->password); 
    bfree(service); 
} 
 
static void* dacast_create(obs_data_t* settings, obs_service_t* service) 
{ 
    UNUSED_PARAMETER(service); 
 
    // blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>create called"); 
 
    rtmp_dacast* data = bzalloc(sizeof(rtmp_dacast)); 
    dacast_update(data, settings); 
 
    return data; 
} 
 
static bool apikey_modified(obs_properties_t* properties, obs_property_t* prop, obs_data_t* settings) 
{ 
    UNUSED_PARAMETER(properties); 
    UNUSED_PARAMETER(prop); 
 
    bfree(apikey_static); 
    apikey_static = bstrdup(obs_data_get_string(settings, "apikey")); 
    return false; 
} 
 
static void sync_property_list(obs_properties_t *props, dacast_channel_array* channels) 
{ 
    obs_property_t* channel_list = obs_properties_get(props, "channel_list"); 
    obs_property_list_clear(channel_list); 
 
    int i; 
    for(i = 0; i < channels->count; i++) 
    { 
        dacast_channel* channel = channels->channels[i]; 
        char* json_repr = channel_to_json(channel); 
        obs_property_list_add_string(channel_list, channel->title, json_repr); 
        free(json_repr); 
    } 
} 
 
static bool refresh_button_clicked(obs_properties_t *props, obs_property_t *property, void* data) 
{ 
    UNUSED_PARAMETER(property); 
    UNUSED_PARAMETER(data); 
 
    dacast_result* result = get_channels(apikey_static); 
    if(result->isError) 
    {
	    blog(LOG_ERROR, "error getting dacast channel list: %s", result->error);
        // txt = result->error; 
        //TODO show error message
	/*if(result->error)
		bfree(result->error); */
    } 
    else 
    { 
        dacast_channel_array* channels = result->channels_array; 
        sync_property_list(props, channels); 
        free_channels(channels); 
    } 
 
    bfree(result); 
 
    return true; 
} 
 
static obs_properties_t* dacast_custom_properties(void* unused) 
{ 
    UNUSED_PARAMETER(unused); 
    // blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>creating properties");     
    obs_properties_t* properties = obs_properties_create(); 
    obs_property_t* apikey_prop; 
 
    apikey_prop = obs_properties_add_text(properties, "apikey", "API key", OBS_TEXT_PASSWORD); 
    obs_property_set_modified_callback(apikey_prop, apikey_modified); 
    obs_properties_add_list(properties, "channel_list", "Channel lists", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING); 
    obs_properties_add_button(properties, "refresh_button", "Refresh list of channels", refresh_button_clicked); 
 
    return properties; 
} 
 
static const char* dacast_get_url(void* data) 
{ 
    rtmp_dacast *service = data; 
    // blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>get url:%s", service->url); 
    return service->url; 
} 
 
static const char* dacast_get_key(void* data) 
{ 
    rtmp_dacast *service = data; 
    // blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>get key:%s", service->key); 
    return service->key; 
} 
 
static const char* dacast_get_username(void *data) 
{ 
    rtmp_dacast *service = data; 
    // blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>get username:%s", service->username); 
    return service->username; 
} 
 
static const char* dacast_get_password(void* data) 
{ 
    rtmp_dacast *service = data; 
    // blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>get password:%s", service->password); 
    return service->password; 
} 
 
struct obs_service_info dacast_service = { 
    .id             = "dacast_rtmp", 
    .get_name       = dacast_get_name, 
    .create         = dacast_create, 
    .destroy        = dacast_destroy, 
    .update         = dacast_update, 
    .get_properties = dacast_custom_properties, 
    .get_url        = dacast_get_url, 
    .get_key        = dacast_get_key, 
    .get_username   = dacast_get_username, 
    .get_password   = dacast_get_password 
};
