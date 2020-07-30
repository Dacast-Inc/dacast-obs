
#include <obs-module.h>
#include <util/circlebuf.h>
#include <util/threading.h>
#include <util/dstr.h>
#include <util/darray.h>
#include <util/platform.h>
#include <curl/curl.h>
#include <sys/stat.h>

#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include "obs-ffmpeg-formats.h"
#include "closest-pixel-format.h"
#include "obs-ffmpeg-compat.h"

//TODO replace concat with sprintf 
//TODO move static members into the struct dacast_hls_output and make a dictionnary of the instances static 
//TODO Move the common curl code into a send_file method


//if there is more than MAX_QUEUED_SEGMENTS queued for sending, the surplus segments are dropped
const int MAX_QUEUED_SEGMENTS = 4;

const char* MUXER_SETTINGS = 
"method=PUT"
"|master_pl_name=chunklist.m3u8"
"|hls_list_size=3"
"|hls_allow_cache=0"
"|http_user_agent=DacastOBS";
//add this to the end "hls_base_url=gjjgjggjgj/"

struct chunk_name {
	char* filename;
	int chunk_nb;
};

typedef	DARRAY(struct chunk_name*) chunk_name_list_t;

struct ffmpeg_cfg {
	const char         *url;
	const char         *format_name;
	const char         *format_mime_type;
	const char         *muxer_settings;
	int                gop_size;
	int                video_bitrate;
	int                audio_bitrate;
	const char         *video_encoder;
	int                video_encoder_id;
	const char         *audio_encoder;
	int                audio_encoder_id;
	const char         *video_settings;
	const char         *audio_settings;
	enum AVPixelFormat format;
	enum AVColorRange  color_range;
	enum AVColorSpace  color_space;
	int                scale_width;
	int                scale_height;
	int                width;
	int                height;
};

struct ffmpeg_data {
	AVStream           *video;
	AVStream           *audio;
	AVCodec            *acodec;
	AVCodec            *vcodec;
	AVFormatContext    *output;
	struct SwsContext  *swscale;

	int64_t            total_frames;
	AVFrame            *vframe;
	int                frame_size;

	uint64_t           start_timestamp;

	int64_t            total_samples;
	uint32_t           audio_samplerate;
	enum audio_format  audio_format;
	size_t             audio_planes;
	size_t             audio_size;
	struct circlebuf   excess_frames[MAX_AV_PLANES];
	uint8_t            *samples[MAX_AV_PLANES];
	AVFrame            *aframe;

	struct ffmpeg_cfg  config;

	bool               initialized;
};

struct dacast_hls_output {
    //state
    bool connecting;
    volatile bool stopping;
	volatile bool      active;

    //metadata
	uint64_t           stop_ts;
    uint64_t           audio_start_ts;
	uint64_t           video_start_ts;
	uint64_t           total_bytes;

    //data 
	obs_output_t       *output;
	struct ffmpeg_data ff_data;
	DARRAY(AVPacket)   packets;
	chunk_name_list_t   segments_to_send_queue;
	const char* url;
	int dropped_frames;

    //threading
	bool               write_thread_active;
	bool		   send_thread_active;

	pthread_t          start_thread;
	pthread_t          send_thread;
	pthread_t          write_thread;
	pthread_mutex_t    write_mutex;
	pthread_mutex_t    send_mutex;
	os_sem_t           *write_sem;
	os_sem_t           *send_sem;
	os_event_t         *stop_event;

};

static char* akamaiSessionId;
static os_event_t *hls_error_event;
static struct dacast_hls_output* outputStatic;

static bool ffmpeg_data_init(struct ffmpeg_data *data, struct ffmpeg_cfg *config);


static char* get_os_program_data_path_clean() {
	struct dstr str = { 0 };
	char* data_path = os_get_program_data_path_ptr("dacast-obs-studio");

	dstr_insert(&str, 0, data_path);
blog(LOG_INFO, "about to bfree(data_path)");
	bfree(data_path);
	dstr_replace(&str, "\\", "/");
	data_path = bstrdup(str.array);
	dstr_free(&str);
	return data_path;
}

char* concat(const char *s1, const char *s2) 
{
    char *result = bzalloc(strlen(s1) + strlen(s2) + 1);//+1 for the null-terminator 
    //in real code you would check for errors in malloc here 
    strncpy(result, s1, strlen(s1)); 
    strcat(result, s2); 
    return result; 
} 

char* rand_string(size_t size)
{
    char *str = bzalloc(size + 1);
    srand(time(NULL));
    if (str) {
        const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        if (size)
        {
            --size;
            for (size_t n = 0; n < size; n++)
            {
                int key = rand() % (int)(sizeof charset - 1);
                str[n] = charset[key];
            }
            str[size] = '\0';
        }
    }
    return str;
}

static const char *dacast_hls_ffmpeg_output_getname(void *unused)
{
    blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>dacast_output_getname");
	UNUSED_PARAMETER(unused);
	return obs_module_text("Dacast HLS");
}


static bool is_interesting_log(int log_level, char* log_entry)
{
    return log_level == AV_LOG_INFO && strstr(log_entry, "chunk_") != NULL;
}

static int extract_chunk_nb(char* filename)
{
    //string should be 'chunk_\d+.ts'
    int i;
    int start_index = -1;
    int end_index = -1;

    for(i = 0; filename[i] != '\0'; i++)
    {
        char c = filename[i];
        if(c == '_'){
            start_index = i + 1;
        }
        if(c == '.'){
            end_index = i;
            break;
        }
    }
    if(start_index == -1 || end_index == -1 || start_index == end_index){
        return -1;
    }

    char* subbuff = bzalloc(sizeof(char)*((end_index - start_index) + 1));
    //char subbuff[(end_index - start_index) + 1];
    memcpy(subbuff, &filename[start_index], end_index - start_index);
    subbuff[(end_index - start_index)] = '\0';

    int parsed = atoi(subbuff);
blog(LOG_INFO, "about to bfree(subbuff)");
    bfree(subbuff);
    return parsed;
}

static bool cleanup_segments(char* dirpath, int cleanup_below)
{
    // blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>cleanup segments below %d", cleanup_below);
    struct os_dirent* dirent;
    os_dir_t* directory;

    directory = os_opendir(dirpath);
    
    if(directory){
        // blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>opened %s", dirpath);
        while( (dirent = os_readdir(directory)) != NULL ){
            if(strstr(dirent->d_name, "chunk_") != NULL){
                int chunk_nb = extract_chunk_nb(dirent->d_name);
                if(chunk_nb > cleanup_below)
                    continue;

                char* filepath_part = concat(dirpath, "/");
                char* filepath = concat(filepath_part, dirent->d_name);

                int unlink_result = os_unlink(filepath);
                // blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>unlinked %s, %d", filepath, unlink_result);
blog(LOG_INFO, "about to bfree(filepath_part)");
                bfree(filepath_part);
blog(LOG_INFO, "about to bfree(filepath)");
                bfree(filepath);
                if(unlink_result != 0){
                    blog(LOG_INFO, "[Process] Couldnt unlink file (%d) %s, result: %d", unlink_result, filepath, unlink_result);
                    //goto fail; dont do anything if it fails?
                }
            }
        }
        os_closedir(directory);
        return true;
    }else{
        // blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>couldnt open directory to cleanup");
        return false;
    }

fail:
    os_closedir(directory);
    return false;
}

static struct chunk_name* get_file_to_send2(char* log_entry){
    int chunk_nb = extract_chunk_nb(log_entry);
    chunk_nb--;
    char* name = bzalloc(sizeof(char)*25);
    sprintf(name, "chunk_%d.ts", chunk_nb);
    struct chunk_name* to_send = bzalloc(sizeof(struct chunk_name));
    to_send->filename = name;
    to_send->chunk_nb = chunk_nb;
    //struct chunk_name to_send = { name, chunk_nb };
    return to_send;
}

/* NOTE: if you want this example to work on Windows with libcurl as a
   DLL, you MUST also provide a read callback with CURLOPT_READFUNCTION.
   Failing to do so will give you a crash since a DLL may not use the
   variable's memory when passed in to it from an app like this. */
static size_t read_callback(void *ptr, size_t size, size_t nmemb, void *stream)
{
	curl_off_t nread;
	/* in real-world cases, this would probably get this data differently
	   as this fread() stuff is exactly what the library already would do
	   by default internally */
	size_t retcode = fread(ptr, size, nmemb, stream);

	nread = (curl_off_t)retcode;

	// fprintf(stderr, "*** We read %" CURL_FORMAT_CURL_OFF_T
	// 	" bytes from file\n", nread);
	return retcode;
}

static bool send_file(char* filename, char* url) {
	blog(LOG_INFO, "[CallStack] send_file(filename=%s, url=%s)", filename, url);
	CURL *curl;
	CURLcode res;
	struct stat file_info;
	// curl_off_t speed_upload, total_time;
	FILE *fd;

	fd = fopen(filename, "rb"); /* open file to upload */
	if (!fd) {
		blog(LOG_INFO, "[ProcessDetail] Couldnt open file '%s'", filename);
		goto fail;
	}
	if (fstat(fileno(fd), &file_info) != 0) {
		blog(LOG_INFO, "[ProcessDetail] Couldnt read file info for '%s'", filename);
		goto fail;
	}

	/* get a curl handle */
	curl = curl_easy_init();
	if (curl) {
		/* First set the URL that is about to receive our POST. This URL can
		just as well be a https:// URL if that is what should receive the
		data. */

		curl_easy_setopt(curl, CURLOPT_URL, url);
		/* tell it to "upload" to the URL */
		curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
		curl_easy_setopt(curl, CURLOPT_PUT, 1L);
		curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);
		/* set where to read from (on Windows you need to use READFUNCTION too) */
		curl_easy_setopt(curl, CURLOPT_READDATA, fd);
		/* and give the size of the upload (optional) */
		curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)file_info.st_size);
		/* enable verbose for easier tracing */
		// curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

		/* Perform the request, res will get the return code */
		res = curl_easy_perform(curl);
		/* Check for errors */
		if (res != CURLE_OK) {
			blog(LOG_INFO, "[ProcessDetail] curl_easy_perform() failed (%d): %s\n",
				res, curl_easy_strerror(res));
			if (res != CURLE_WRITE_ERROR) {
				blog(LOG_INFO, "[ProcessDetail] CURLE_WRITE_ERROR for %s", filename);
				goto fail;
			}
		}
		else {
			blog(LOG_INFO, "[ProcessDetail] curl_easy_perform() was successful for %s", filename);
			/* now extract transfer info */
			// curl_easy_getinfo(curl, CURLINFO_SPEED_UPLOAD_T, &speed_upload);
			// curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &total_time);

			// blog(LOG_INFO, "Speed: %" CURL_FORMAT_CURL_OFF_T " bytes/sec during %"
			//         CURL_FORMAT_CURL_OFF_T ".%06ld seconds\n",
			//         speed_upload,
			//         (total_time / 1000000), (long)(total_time % 1000000));
		}

		/* always cleanup */
		curl_easy_cleanup(curl);
	}
	fclose(fd);
	return true;

fail:
	if (fd) {
		fclose(fd);
	}
	if (curl) {
		curl_easy_cleanup(curl);
	}
	return false;
}

static bool send_m3u8(char* ingest_url)
{
	blog(LOG_INFO, "[CallStack] send_m3u8(ingest_url=%s)", ingest_url);

	char* url = bzalloc(strlen(ingest_url) + strlen("chunklist.m3u8") + 1);//+1 for the null-terminator 
	sprintf(url, "%schunklist.m3u8", ingest_url);

	char* cwd = get_os_program_data_path_clean();
	char* filename = bzalloc(strlen(cwd) + strlen("/chunklist.m3u8") + 1);//+1 for the null-terminator 
	sprintf(filename, "%s/chunklist.m3u8", cwd);

	bool result = send_file(filename, url);
blog(LOG_INFO, "about to bfree(url)");
	bfree(url);
blog(LOG_INFO, "about to bfree(filename)");
	bfree(filename);
blog(LOG_INFO, "about to bfree(cwd)");
	bfree(cwd);
	return result;
}

static bool send_segment(char* ingest_url, struct chunk_name* to_send, char* sessionId)
{
	blog(LOG_INFO, "[CallStack] send_segment(ingest_url=%s, to_send=%p, sessionId=%s)", ingest_url, to_send, sessionId);

	char* url = bzalloc(strlen(ingest_url) + strlen(sessionId) + 1/*for '/'*/ + strlen(to_send->filename) + 1);//+1 for the null-terminator 
	sprintf(url, "%s%s/%s", ingest_url, sessionId, to_send->filename);

	char* cwd = get_os_program_data_path_clean();
	char* filepath = bzalloc(strlen(cwd) + strlen("/") + strlen(to_send->filename) + 1);//+1 for the null-terminator 
	sprintf(filepath, "%s/%s", cwd, to_send->filename);

	bool result = send_file(filepath, url);

blog(LOG_INFO, "about to bfree(url)");
	bfree(url);
blog(LOG_INFO, "about to bfree(filepath)");
	bfree(filepath);
blog(LOG_INFO, "about to bfree(cwd)");
	bfree(cwd);
	return result;
}


static void ffmpeg_log_callback(void *param, int level, const char *format,
		va_list args)
{
    if (level <= AV_LOG_INFO){
        // blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>dacast_ffmpeg_log_callback");

    char out[4096];
	char* format_cp = bstrdup(format);
    vsnprintf(out, sizeof(out), format_cp, args);
blog(LOG_INFO, "about to bfree(format_cp)");
	bfree(format_cp);
        blog(LOG_INFO, "log %d %s", level, out);

        if(is_interesting_log(level, out)){
            struct chunk_name* to_send = get_file_to_send2(out);
            if(to_send->chunk_nb == -1){
                return;
            }
            blog(LOG_INFO, "[Process] Found chunk to send filename=%s, chunk_nb=%d", to_send->filename, to_send->chunk_nb);

			pthread_mutex_lock(&outputStatic->send_mutex);
			da_push_back(outputStatic->segments_to_send_queue, &to_send);
			pthread_mutex_unlock(&outputStatic->send_mutex);

			os_sem_post(outputStatic->send_sem);
        }
    }
	UNUSED_PARAMETER(param);
}

static void *dacast_hls_ffmpeg_output_create(obs_data_t *settings, obs_output_t *output)
{
    blog(LOG_INFO, "[CallStack] dacast_hls_ffmpeg_output_create(settings=%p, output=%p)", settings, output);
	struct dacast_hls_output *data = bzalloc(sizeof(struct dacast_hls_output));
	pthread_mutex_init_value(&data->write_mutex);
	pthread_mutex_init_value(&data->send_mutex);
	data->output = output;
	data->dropped_frames = 0;

	if (pthread_mutex_init(&data->write_mutex, NULL) != 0)
		goto fail;
	if (pthread_mutex_init(&data->send_mutex, NULL) != 0)
		goto fail;
	if (os_event_init(&data->stop_event, OS_EVENT_TYPE_AUTO) != 0)
		goto fail;
    blog(LOG_INFO, "[ProcessDetail] init hls_error_event");
	if(os_event_init(&hls_error_event, OS_EVENT_TYPE_AUTO) != 0)
		goto fail;
    blog(LOG_INFO, "[ProcessDetail] sucessfully init hls_error_event");
	if (os_sem_init(&data->write_sem, 0) != 0)
		goto fail;
	if (os_sem_init(&data->send_sem, 0) != 0)
		goto fail;

	av_log_set_callback(ffmpeg_log_callback);

	UNUSED_PARAMETER(settings);
	UNUSED_PARAMETER(output);
	return data;

fail:
	pthread_mutex_destroy(&data->write_mutex);
	pthread_mutex_destroy(&data->send_mutex);
	os_event_destroy(data->stop_event);
    blog(LOG_INFO, "[ProcessDetail] destroy hls_error_event, dacast_hls_ffmpeg_output_create");
    os_event_destroy(hls_error_event);
    hls_error_event = NULL;
blog(LOG_INFO, "about to bfree(data)");
	bfree(data);
	return NULL;
}


static void close_video(struct ffmpeg_data *data)
{
    blog(LOG_INFO, "[CallStack] close_video(data=%p)", data);
	avcodec_close(data->video->codec);
	av_frame_unref(data->vframe);

	// This format for some reason derefs video frame
	// too many times
	if (data->vcodec->id == AV_CODEC_ID_A64_MULTI ||
	    data->vcodec->id == AV_CODEC_ID_A64_MULTI5)
		return;

	av_frame_free(&data->vframe);
}

static void close_audio(struct ffmpeg_data *data)
{
    blog(LOG_INFO, "[CallStack] close_audio(data=%p)", data);
	for (size_t i = 0; i < MAX_AV_PLANES; i++)
		circlebuf_free(&data->excess_frames[i]);

	av_freep(&data->samples[0]);
	avcodec_close(data->audio->codec);
	av_frame_free(&data->aframe);
}

static void ffmpeg_data_free(struct ffmpeg_data *data)
{
    blog(LOG_INFO, "[CallStack] ffmpeg_data_free(data=%p)", data);
	if (data->initialized)
		av_write_trailer(data->output);

	if (data->video)
		close_video(data);
	if (data->audio)
		close_audio(data);

	if (data->config.muxer_settings)
		blog;

	if (data->output) {
		if ((data->output->oformat->flags & AVFMT_NOFILE) == 0)
			avio_close(data->output->pb);

		avformat_free_context(data->output);
	}

	memset(data, 0, sizeof(struct ffmpeg_data));
}

static void ffmpeg_deactivate(struct dacast_hls_output *output)
{
    blog(LOG_INFO, "[CallStack] ffmpeg_deactivate(output=%p)", output);
    if (output->write_thread_active || output->send_thread_active) {
	    os_event_signal(output->stop_event);
    }
	if (output->write_thread_active) {
		os_sem_post(output->write_sem);
		pthread_join(output->write_thread, NULL);
		output->write_thread_active = false;
	}
	if (output->send_thread_active) {
		os_sem_post(output->send_sem);
		pthread_join(output->send_thread, NULL);
		output->send_thread_active = false;
	}

	pthread_mutex_lock(&output->write_mutex);

	for (size_t i = 0; i < output->packets.num; i++)
		av_free_packet(output->packets.array+i);
	da_free(output->packets);
	pthread_mutex_unlock(&output->write_mutex);

	pthread_mutex_lock(&output->send_mutex);
	for (size_t i = 0; i < output->segments_to_send_queue.num; i++) {
		struct chunk_name** chunk = output->segments_to_send_queue.array + i;
blog(LOG_INFO, "about to bfree((*chunk)->filename)");
		bfree((*chunk)->filename);
blog(LOG_INFO, "about to bfree(*chunk)");
		bfree(*chunk);
	}
	da_free(output->segments_to_send_queue);
	pthread_mutex_unlock(&output->send_mutex);


	ffmpeg_data_free(&output->ff_data);
}

static void ffmpeg_output_full_stop(void *data)
{
    blog(LOG_INFO, "[CallStack] ffmpeg_output_full_stop(data=%p)", data);
	struct dacast_hls_output *output = data;

	if (output->active) {
		obs_output_end_data_capture(output->output);
		ffmpeg_deactivate(output);
	}
}

static void dacast_hls_ffmpeg_output_destroy(void *data)
{
    blog(LOG_INFO, "[CallStack] dacast_hls_ffmpeg_output_destroy(data=%p)", data);
	struct dacast_hls_output *output = data;

    if(akamaiSessionId){
blog(LOG_INFO, "about to bfree(akamaiSessionId)");
        bfree(akamaiSessionId);
        akamaiSessionId = NULL;
    }

	if (output) {
		if (output->connecting)
			pthread_join(output->start_thread, NULL);

		ffmpeg_output_full_stop(output);

		pthread_mutex_destroy(&output->write_mutex);
		pthread_mutex_destroy(&output->send_mutex);
		os_sem_destroy(output->write_sem);
		os_sem_destroy(output->send_sem);
		os_event_destroy(output->stop_event);
        blog(LOG_INFO, "[ProcessDetail] destroy hls_error_event, dacast_hls_ffmpeg_output_destroy");
        os_event_destroy(hls_error_event);
	hls_error_event = NULL;
blog(LOG_INFO, "about to bfree(output->url)");
        bfree(output->url);
blog(LOG_INFO, "about to bfree(data)");
		bfree(data);
        outputStatic = NULL;
	}
}

static inline const char *get_string_or_null(obs_data_t *settings,
		const char *name)
{
	const char *value = obs_data_get_string(settings, name);
	if (!value || !strlen(value))
		return NULL;
	return value;
}

static inline bool stopping(struct dacast_hls_output *output)
{
    // blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>stopping");
	return os_atomic_load_bool(&output->stopping);
}


static uint64_t get_packet_sys_dts(struct dacast_hls_output *output,
		AVPacket *packet)
{
    // blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>get_packet_sys_dts");
	struct ffmpeg_data *data = &output->ff_data;
	uint64_t start_ts;

	AVRational time_base;

	if (data->video && data->video->index == packet->stream_index) {
		time_base = data->video->time_base;
		start_ts = output->video_start_ts;
	} else if(data->audio){
		time_base = data->audio->time_base;
		start_ts = output->audio_start_ts;
	} else {
		blog(LOG_INFO, "[ProcessDetail] get_packet_sys_dts returning 0 because data->audio is null %p", data->audio);
		return 0;
	}

    uint64_t dts = start_ts + (uint64_t)av_rescale_q(packet->dts,
			time_base, (AVRational){1, 1000000000});

    // blog(LOG_INFO, "made dts=%" PRIu64, dts);
	return dts;
}

static int process_packet(struct dacast_hls_output *output)
{
    // blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>process_packet");
	AVPacket packet;
	bool new_packet = false;
	int ret;

	pthread_mutex_lock(&output->write_mutex);
	if (output->packets.num) {
		packet = output->packets.array[0];
		//blog(LOG_INFO, "[ProcessDetail] Got new packet, buf addr=%p", packet.buf);
		da_erase(output->packets, 0);
		new_packet = true;
	}
	pthread_mutex_unlock(&output->write_mutex);

	if (!new_packet)
		return 0;

	/*blog(LOG_INFO, "size = %d, flags = %lX, stream = %d, "
			"packets queued: %lu",
			packet.size, packet.flags,
			packet.stream_index, output->packets.num);*/

	if (stopping(output)) {
		uint64_t sys_ts = get_packet_sys_dts(output, &packet);
		if (sys_ts >= output->stop_ts) {
			ffmpeg_output_full_stop(output);
			return 0;
		}
	}
	//blog(LOG_INFO, "[ProcessDetail] get_packet_sys_dts happened, buf addr is now=%p", packet.buf);


	output->total_bytes += packet.size;


	ret = av_interleaved_write_frame(output->ff_data.output, &packet);
	if (ret < 0) {
		av_free_packet(&packet);
		blog(LOG_WARNING, "receive_audio: Error writing packet: %s",
				av_err2str(ret));
		return ret;
	}

	return 0;
}

static void *write_thread(void *data)
{
    blog(LOG_INFO, "[CallStack] write_thread(data=%p)", data);
    struct dacast_hls_output *output = data;

	while (os_sem_wait(output->write_sem) == 0) {
		/* check to see if shutting down */
		if (os_event_try(output->stop_event) == 0)
			break;
	    // blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>write thread triggered");


        bool has_hls_error = os_event_try(hls_error_event) == 0;
        int ret = 0;
		ret = process_packet(output);
		if (ret != 0 || has_hls_error) {
			blog(LOG_INFO, "[Process] Write thread stopping on error. ret=%d, has_hls_error=%d", ret, has_hls_error);
			int code = OBS_OUTPUT_ERROR;

			pthread_detach(output->write_thread);
			output->write_thread_active = false;

			if (ret == -ENOSPC)
				code = OBS_OUTPUT_NO_SPACE;

			obs_output_signal_stop(output->output, code);
			ffmpeg_deactivate(output);
			break;
		}
	}

	output->active = false;
	return NULL;
}

/** this runs in the send_thread */
static bool send_queued_segments(struct dacast_hls_output *output, chunk_name_list_t send_queue, int* cleanup_before) {
	blog(LOG_INFO, "[CallStack] send_queued_segments(output=%p, send_queue=%p, cleanup_before=%p)", output, send_queue, cleanup_before);
	
	int dropped_segments = 0;
	while (send_queue.num > MAX_QUEUED_SEGMENTS) {
		struct chunk_name** to_delete_ptr = send_queue.array;
		struct chunk_name* to_delete = *to_delete_ptr;
		da_erase(send_queue, 0);
blog(LOG_INFO, "about to bfree(to_delete->filename)");
		bfree(to_delete->filename);
blog(LOG_INFO, "about to bfree(to_delete)");
		bfree(to_delete);
		dropped_segments++;
	}
	if (dropped_segments > 0) {
		output->dropped_frames += output->ff_data.config.gop_size*dropped_segments;
		blog(LOG_INFO, "[Process] Too many segments queued, dropped %d segments", dropped_segments);
	}

	int highest_segment = -1;

	while (true) {
		if (!send_queue.num) {
			break;
		}
		struct chunk_name** to_send_ptr = send_queue.array;
		struct chunk_name* to_send = *to_send_ptr;
		da_erase(send_queue, 0);
		blog(LOG_INFO, "[Process] Sending segment number=%d, name=%s, queued=%d, url=%s", to_send->chunk_nb, to_send->filename, send_queue.num, output->url);

		if (!send_segment(output->url, to_send, akamaiSessionId)) {
			blog("error sending segment %s nb %d", to_send->filename, to_send->chunk_nb);
			os_event_signal(hls_error_event);
			break;
		}

		if (highest_segment < to_send->chunk_nb) {
			highest_segment = to_send->chunk_nb;
		}
		blog(LOG_INFO, "[Process] Sent segment number=%d, name=%s, queued=%d, url=%s", to_send->chunk_nb, to_send->filename, send_queue.num, output->url);
blog(LOG_INFO, "about to bfree(to_send->filename)");
		bfree(to_send->filename);
blog(LOG_INFO, "about to bfree(to_send)");
		bfree(to_send);
	}
	*cleanup_before = highest_segment;
	return true;
}

static void* send_thread(void* data) {
	blog(LOG_INFO, "[CallStack] send_thread(data=%p)", data);
	struct dacast_hls_output *output = data;
	blog(LOG_INFO, "[ProcessDetails] casted data to output");
	while (os_sem_wait(output->send_sem) == 0) {
		if (os_event_try(output->stop_event) == 0) 
			break;
		blog(LOG_INFO, "[Process] Send thread trigerred");

		int cleanup_before = -1;
		chunk_name_list_t send_queue;
		da_init(send_queue);

		//copy the list so the mutex isnt blocked while doing the curl
		pthread_mutex_lock(&output->send_mutex);
		da_copy(send_queue, output->segments_to_send_queue);
		while (output->segments_to_send_queue.num)
			da_erase(output->segments_to_send_queue, 0);
		pthread_mutex_unlock(&output->send_mutex);

		bool sent_sucessfully = send_queued_segments(output, send_queue, &cleanup_before);
		da_free(send_queue);

		if (!sent_sucessfully) {
			pthread_detach(output->send_thread);
			output->send_thread_active = false;
			obs_output_signal_stop(output->output, OBS_OUTPUT_ERROR);
			ffmpeg_deactivate(output);
			break;
		}

		sent_sucessfully = send_m3u8(output->url);

		if (!sent_sucessfully) {
			pthread_detach(output->send_thread);
			output->send_thread_active = false;
			obs_output_signal_stop(output->output, OBS_OUTPUT_ERROR);
			ffmpeg_deactivate(output);
			break;
		}

		if (cleanup_before >= 0) {
			char* cwd = get_os_program_data_path_clean();
			bool cleanup_result = cleanup_segments(cwd, cleanup_before - 3);
blog(LOG_INFO, "about to bfree(cwd)");
			bfree(cwd);
			if (!cleanup_result) {
				blog(LOG_ERROR, "[ProcessDetail] Error cleaning up segment below %d", cleanup_before - 3);
			}
		}
		
		
	}
	output->active = false;
	return NULL;
}

// You must free the result if result is non-NULL.
char *str_replace(char *orig, char *rep, char *with) {
	char *result; // the return string
	char *ins;    // the next insert point
	char *tmp;    // varies
	int len_rep;  // length of rep (the string to remove)
	int len_with; // length of with (the string to replace rep with)
	int len_front; // distance between rep and end of last rep
	int count;    // number of replacements

	// sanity checks and initialization
	if (!orig || !rep)
		return NULL;
	len_rep = strlen(rep);
	if (len_rep == 0)
		return NULL; // empty rep causes infinite loop during count
	if (!with)
		with = "";
	len_with = strlen(with);

	// count the number of replacements needed
	ins = orig;
	for (count = 0; tmp = strstr(ins, rep); ++count) {
		ins = tmp + len_rep;
	}

	tmp = result = malloc(strlen(orig) + (len_with - len_rep) * count + 1);

	if (!result)
		return NULL;

	// first time through the loop, all the variable are set correctly
	// from here on,
	//    tmp points to the end of the result string
	//    ins points to the next occurrence of rep in orig
	//    orig points to the remainder of orig after "end of rep"
	while (count--) {
		ins = strstr(orig, rep);
		len_front = ins - orig;
		tmp = strncpy(tmp, orig, len_front) + len_front;
		tmp = strcpy(tmp, with) + len_with;
		orig += len_front + len_rep; // move to next "end of rep"
	}
	strcpy(tmp, orig);
	return result;
}

static bool try_connect(struct dacast_hls_output *output)
{
    blog(LOG_INFO, "[CallStack] try_connect(output=%p)", output);
	video_t *video = obs_output_video(output->output);
	const struct video_output_info *voi = video_output_get_info(video);
	struct ffmpeg_cfg config;
	obs_data_t *settings;
    char* muxer_settings;
	bool success;
	int ret;

	settings = obs_output_get_settings(output->output);

	// obs_data_set_default_int(settings, "gop_size", voi->fps_num*2); dd: not sure what this was used for?

// " hls_time=2"
// " hls_init_time=2"
	char* data_path = get_os_program_data_path_clean();

	//char* data_path = str_replace(data_path_dirty, "\\", "/");
blog(LOG_INFO, "about to bfree(data_path_dirty)");
	//bfree(data_path_dirty);

	char* session_id = rand_string(10);
	if (akamaiSessionId) {
blog(LOG_INFO, "about to bfree(akamaiSessionId)");
		bfree(akamaiSessionId);
		akamaiSessionId = NULL;
	}
	akamaiSessionId = bstrdup(session_id);


	int keyframeIntervalSec = (int)obs_data_get_int(settings, "hls_keyframe_interval");
	muxer_settings = bzalloc(
		strlen(MUXER_SETTINGS)
		+ strlen("|hls_segment_filename=")
		+ strlen(data_path)
		+ strlen("/chunk_%0d.ts")//"|hls_segment_filename=/Library/Application Support/obs-studio/chunk_%0d.ts"
		+ strlen("|hls_time=")
		+ (sizeof(char) * 3) //keyframe length
		+ strlen("|hls_init_time=")
		+ (sizeof(char) * 3) //keyframe length
		+ strlen("|hls_base_url=")
		+ (sizeof(char) * 10) // akamaiSessionId
		+ strlen("/")
		+ 1);//+1 for the null-terminator 
	sprintf(
		//%% escapes the %
		muxer_settings, "%s|hls_segment_filename=%s/chunk_%%0d.ts|hls_time=%d|hls_init_time=%d|hls_base_url=%s/", 
		MUXER_SETTINGS, 
		data_path, 
		keyframeIntervalSec, 
		keyframeIntervalSec, 
		session_id
	);
blog(LOG_INFO, "about to bfree(session_id)");
    bfree(session_id);

    /*
url: http://post.dctranslive01-i.akamaihd.net/266820/live-104301-474912_1_1/chunklist.m3u8
format_name: hls
format_mime_type: (null)
muxer_settings: method=PUT master_pl_name=chunklist.m3u8 hls_segment_filename=chunk_%0d.ts hls_time=2 hls_init_time=2 g=2 hls_list_size=0 hls_base_url=gjjgjggjgj/ hls_list_size=3 hls_allow_cache=0 http_user_agent=DacastOBS
video_bitrate: 2500
audio_bitrate: 160
gop_size: 60
video_encoder: libx264
video_encoder_id: 27
video_settings: 
audio_encoder: aac
audio_encoder_id: 86018
audio_settings: 
scale_width: 1920
scale_height: 1080
width: 1920
height: 1080
format: 23
    */

//"http://post.dctranslive01-i.akamaihd.net/266820/live-104301-474912_1_1/chunklist.m3u8";
    char* hls_ingest_url = bstrdup(obs_data_get_string(settings, "hls_ingest_url"));

	blog(LOG_INFO, "[ProcessDetails] Setting muxer_settings: %s", muxer_settings);

    output->url = hls_ingest_url;
    config.url = concat(data_path, "/chunklist.m3u8");
blog(LOG_INFO, "about to bfree(data_path)");
    bfree(data_path);
	config.format_name = "hls";
	config.format_mime_type = NULL;
	config.muxer_settings = bstrdup(muxer_settings);
blog(LOG_INFO, "about to bfree(muxer_settings)");
	bfree(muxer_settings);
	config.video_bitrate = (int)obs_data_get_int(settings, "hls_video_bitrate");
	config.audio_bitrate = (int)obs_data_get_int(settings, "hls_audio_bitrate");
	config.gop_size = voi->fps_num*keyframeIntervalSec;
	config.video_encoder = "libx264";//get_string_or_null(settings, "video_encoder");
	config.video_encoder_id = 27;//(int)obs_data_get_int(settings, "video_encoder_id");
	config.audio_encoder = "aac";//get_string_or_null(settings, "audio_encoder");
	config.audio_encoder_id = 86018;//(int)obs_data_get_int(settings,"audio_encoder_id");
	config.video_settings = "";//obs_data_get_string(settings, "video_settings");
	config.audio_settings = "";//obs_data_get_string(settings, "audio_settings");
	config.scale_width = (int)obs_data_get_int(settings, "hls_scale_width");
	config.scale_height = (int)obs_data_get_int(settings, "hls_scale_height");
	config.width  = (int)obs_output_get_width(output->output);
	config.height = (int)obs_output_get_height(output->output);
	config.format = obs_to_ffmpeg_video_format(video_output_get_format(video));

	if (format_is_yuv(voi->format)) {
		config.color_range = voi->range == VIDEO_RANGE_FULL ?
			AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
		config.color_space = voi->colorspace == VIDEO_CS_709 ?
			AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
	} else {
		config.color_range = AVCOL_RANGE_UNSPECIFIED;
		config.color_space = AVCOL_SPC_RGB;
	}

	if (config.format == AV_PIX_FMT_NONE) {
		blog(LOG_DEBUG, "invalid pixel format used for FFmpeg output");
		return false;
	}

	if (!config.scale_width)
		config.scale_width = config.width;
	if (!config.scale_height)
		config.scale_height = config.height;

    blog(LOG_INFO, "configs:\n"
        "url: %s\n"
        "format_name: %s\n"
        "format_mime_type: %s\n"
        "muxer_settings: %s\n"
        "video_bitrate: %d\n"
        "audio_bitrate: %d\n"
        "gop_size: %d\n"
        "video_encoder: %s\n"
        "video_encoder_id: %d\n"
        "video_settings: %s\n"
        "audio_encoder: %s\n"
        "audio_encoder_id: %d\n"
        "audio_settings: %s\n"
        "scale_width: %d\n"
        "scale_height: %d\n"
        "width: %d\n"
        "height: %d\n"
        "format: %d\n",
        config.url,
        config.format_name,
        config.format_mime_type,
        config.muxer_settings,
        config.video_bitrate,
        config.audio_bitrate,
        config.gop_size,
        config.video_encoder,
        config.video_encoder_id,
        config.video_settings,
        config.audio_encoder,
        config.audio_encoder_id,
        config.audio_settings,
        config.scale_width,
        config.scale_height,
        config.width,
        config.height,
        config.format
    );

	success = ffmpeg_data_init(&output->ff_data, &config);
	obs_data_release(settings);

	if (!success)
		return false;

	struct audio_convert_info aci = {
		.format = output->ff_data.audio_format
	};

	output->active = true;

	if (!obs_output_can_begin_data_capture(output->output, 0))
		return false;

	ret = pthread_create(&output->write_thread, NULL, write_thread, output);
	blog(LOG_INFO, "[ProcessDetails] created write_thread done");
	if (ret != 0) {
		blog(LOG_WARNING, "ffmpeg_output_start: failed to create write "
		                  "thread.");
		ffmpeg_output_full_stop(output);
		return false;
	}
	ret = pthread_create(&output->send_thread, NULL, send_thread, output);
	blog(LOG_INFO, "[ProcessDetails] created send_thread done");
	if (ret != 0) {
		blog(LOG_WARNING, "ffmpeg_output_start: failed to create send thread.");
		ffmpeg_output_full_stop(output);
		return false;
	}


	obs_output_set_video_conversion(output->output, NULL);
	obs_output_set_audio_conversion(output->output, &aci);
	obs_output_begin_data_capture(output->output, 0);
	output->write_thread_active = true;
	output->send_thread_active = true;
	blog(LOG_INFO, "[ProcessDetails] try_connect done");
	return true;
}

static void *start_thread(void *data)
{
    blog(LOG_INFO, "[CallStack] start_thread(data=%p)", data);
	struct dacast_hls_output *output = data;

	char* cwd = get_os_program_data_path_clean();
    blog(LOG_INFO, "[ProcessDetail] Cwd: %s", cwd);
    bool cleanup_result = cleanup_segments(cwd, INT_MAX);
blog(LOG_INFO, "about to bfree(cwd)");
    bfree(cwd);
    if(!cleanup_result){
        obs_output_signal_stop(output->output, OBS_OUTPUT_CONNECT_FAILED);
	    output->connecting = false;
        return NULL;
    }

	if (!try_connect(output))
		obs_output_signal_stop(output->output, OBS_OUTPUT_CONNECT_FAILED);

	output->connecting = false;
    blog(LOG_INFO, "[ProcessDetail] start_thread done");
	return NULL;
}

static bool dacast_hls_ffmpeg_output_start(void *data)
{
	blog(LOG_INFO, "[CallStack] dacast_hls_ffmpeg_output_start(data=%p)", data);
	struct dacast_hls_output *output = data;
	outputStatic = output;
	int ret;

	if (output->connecting)
		return false;

    if(!hls_error_event && os_event_init(&hls_error_event, OS_EVENT_TYPE_AUTO) != 0){
        blog(LOG_INFO, "[ProcessDetail] error init hls_error_event, destroy hls_error_event, dacast_hls_ffmpeg_output_start");
        os_event_destroy(hls_error_event);
        hls_error_event = NULL;
        return false;
    }
		

	os_atomic_set_bool(&output->stopping, false);
	output->audio_start_ts = 0;
	output->video_start_ts = 0;
	output->total_bytes = 0;

	ret = pthread_create(&output->start_thread, NULL, start_thread, output);
    output->connecting = (ret == 0);
	blog(LOG_INFO, "[CallStack] dacast_hls_ffmpeg_output_start done");
	return output->connecting;
}



static void dacast_hls_ffmpeg_output_stop(void *data, uint64_t ts)
{
	blog(LOG_INFO, "[CallStack] dacast_hls_ffmpeg_output_stop(data=%p, ts=%" PRId64 ")", data, ts);
	struct dacast_hls_output *output = data;

	if (output->active) {
		if (ts == 0) {
			ffmpeg_output_full_stop(output);
		} else {
			os_atomic_set_bool(&output->stopping, true);
			output->stop_ts = ts;
		}
	}
}

static enum AVCodecID get_codec_id(const char *name, int id)
{
	blog(LOG_INFO, "[CallStack] get_codec_id(name=%s, id=%d)", name, id);
	AVCodec *codec;

	if (id != 0)
		return (enum AVCodecID)id;

	if (!name || !*name)
		return AV_CODEC_ID_NONE;

	codec = avcodec_find_encoder_by_name(name);
	if (!codec)
		return AV_CODEC_ID_NONE;

	return codec->id;
}

static void set_encoder_ids(struct ffmpeg_data *data)
{
	blog(LOG_INFO, "[CallStack] set_encoder_ids(data=%p)", data);
	data->output->oformat->video_codec = avcodec_find_encoder_by_name("libx264")->id;
		/*get_codec_id(
			data->config.video_encoder,
			data->config.video_encoder_id);*/

	data->output->oformat->audio_codec = avcodec_find_encoder_by_name("aac")->id;
		/*get_codec_id(
			data->config.audio_encoder,
			data->config.audio_encoder_id);*/
}

static bool parse_params(AVCodecContext *context, char **opts)
{
	blog(LOG_INFO, "[CallStack] parse_params(context=%p, opts=%p)", context, opts);
	bool ret = true;

	if (!context || !context->priv_data)
		return true;

	while (*opts) {
		char *opt = *opts;
		char *assign = strchr(opt, '=');

		if (assign) {
			char *name = opt;
			char *value;

			*assign = 0;
			value = assign+1;

			if (av_opt_set(context->priv_data, name, value, 0)) {
				blog(LOG_WARNING, "Failed to set %s=%s", name, value);
				ret = false;
			}
		}

		opts++;
	}

	return ret;
}

static bool new_stream(struct ffmpeg_data *data, AVStream **stream,
		AVCodec **codec, enum AVCodecID id, const char *name)
{
	blog(LOG_INFO, "[CallStack] new_stream(data=%p, stream=%p, codec=%p, id=%d, name=%s)", data, stream, codec, id, name);

	*codec = (!!name && *name) ?
		avcodec_find_encoder_by_name(name) :
		avcodec_find_encoder(id);

	if (!*codec) {
		blog(LOG_WARNING, "Couldn't find encoder '%s'",
				avcodec_get_name(id));
		return false;
	}

	*stream = avformat_new_stream(data->output, *codec);
	if (!*stream) {
		blog(LOG_WARNING, "Couldn't create stream for encoder '%s'",
				avcodec_get_name(id));
		return false;
	}

	(*stream)->id = data->output->nb_streams-1;
	return true;
}

static bool open_video_codec(struct ffmpeg_data *data)
{
	blog(LOG_INFO, "[CallStack] open_video_codec(data=%p)", data);
	AVCodecContext *context = data->video->codec;
	char **opts = strlist_split(data->config.video_settings, ' ', false);
	int ret;

	if (strcmp(data->vcodec->name, "libx264") == 0)
		av_opt_set(context->priv_data, "preset", "veryfast", 0);

	if (opts) {
		// libav requires x264 parameters in a special format which may be non-obvious
		if (!parse_params(context, opts) && strcmp(data->vcodec->name, "libx264") == 0)
			blog(LOG_WARNING, "If you're trying to set x264 parameters, use x264-params=name=value:name=value");
		strlist_free(opts);
	}

	ret = avcodec_open2(context, data->vcodec, NULL);
	if (ret < 0) {
		blog(LOG_WARNING, "Failed to open video codec: %s",
				av_err2str(ret));
		return false;
	}

	data->vframe = av_frame_alloc();
	if (!data->vframe) {
		blog(LOG_WARNING, "Failed to allocate video frame");
		return false;
	}

	data->vframe->format = context->pix_fmt;
	data->vframe->width  = context->width;
	data->vframe->height = context->height;
	data->vframe->colorspace = data->config.color_space;
	data->vframe->color_range = data->config.color_range;

	ret = av_frame_get_buffer(data->vframe, base_get_alignment());
	if (ret < 0) {
		blog(LOG_WARNING, "Failed to allocate vframe: %s",
				av_err2str(ret));
		return false;
	}

	return true;
}

static bool open_audio_codec(struct ffmpeg_data *data)
{
	blog(LOG_INFO, "[CallStack] open_audio_codec(data=%p)", data);
	AVCodecContext *context = data->audio->codec;
	char **opts = strlist_split(data->config.audio_settings, ' ', false);
	int ret;

	if (opts) {
		parse_params(context, opts);
		strlist_free(opts);
	}

	data->aframe = av_frame_alloc();
	if (!data->aframe) {
		blog(LOG_WARNING, "Failed to allocate audio frame");
		return false;
	}

	context->strict_std_compliance = -2;

	ret = avcodec_open2(context, data->acodec, NULL);
	if (ret < 0) {
		blog(LOG_WARNING, "Failed to open audio codec: %s",
				av_err2str(ret));
		return false;
	}

	data->frame_size = context->frame_size ? context->frame_size : 1024;

	ret = av_samples_alloc(data->samples, NULL, context->channels,
			data->frame_size, context->sample_fmt, 0);
	if (ret < 0) {
		blog(LOG_WARNING, "Failed to create audio buffer: %s",
		                av_err2str(ret));
		return false;
	}

	return true;
}

static bool init_swscale(struct ffmpeg_data *data, AVCodecContext *context)
{
	blog(LOG_INFO, "[CallStack] init_swscale(data=%p, context=%p)", data, context);
	data->swscale = sws_getContext(
			data->config.width, data->config.height,
			data->config.format,
			data->config.scale_width, data->config.scale_height,
			context->pix_fmt,
			SWS_BICUBIC, NULL, NULL, NULL);

	if (!data->swscale) {
		blog(LOG_WARNING, "dacast Could not initialize swscale");
		return false;
	}

	return true;
}

static bool create_video_stream(struct ffmpeg_data *data)
{
	blog(LOG_INFO, "[CallStack] create_video_stream(data=%p)", data);
	enum AVPixelFormat closest_format;
	AVCodecContext *context;
	struct obs_video_info ovi;

	if (!obs_get_video_info(&ovi)) {
		blog(LOG_WARNING, "No active video");
		return false;
	}

	if (!new_stream(data, &data->video, &data->vcodec,
				data->output->oformat->video_codec,
				data->config.video_encoder))
		return false;

	closest_format = get_closest_format(data->config.format,
			data->vcodec->pix_fmts);

	context                 = data->video->codec;
	context->bit_rate       = data->config.video_bitrate * 1000;
    context->bit_rate_tolerance = 0;
    av_opt_set(context->priv_data, "bitrate", data->config.video_bitrate * 1000, AV_OPT_SEARCH_CHILDREN);
    av_opt_set(context->priv_data, "vbv-maxrate", data->config.video_bitrate * 1000, AV_OPT_SEARCH_CHILDREN);
    av_opt_set(context->priv_data, "vbv-bufsize", (data->config.video_bitrate * 1000)/2, AV_OPT_SEARCH_CHILDREN);
    av_opt_set(context->priv_data, "pass", 2, AV_OPT_SEARCH_CHILDREN);
	context->width          = data->config.scale_width;
	context->height         = data->config.scale_height;
	context->time_base      = (AVRational){ ovi.fps_den, ovi.fps_num };
	context->gop_size       = data->config.gop_size;
	context->pix_fmt        = closest_format;
	context->colorspace     = data->config.color_space;
	context->color_range    = data->config.color_range;
	context->thread_count   = 0;
	//data->output->oformat->flags |= AVFMT_NOTIMESTAMPS; //added that because of a bug that said
	//Application provided invalid, non monotonically increasing dts to muxer in stream 0: 1356000 >= 1347000
	//probably not a real fix

	data->video->time_base = context->time_base;

	if (data->output->oformat->flags & AVFMT_GLOBALHEADER)
		context->flags |= CODEC_FLAG_GLOBAL_H;

	if (!open_video_codec(data))
		return false;

	if (context->pix_fmt    != data->config.format ||
	    data->config.width  != data->config.scale_width ||
	    data->config.height != data->config.scale_height) {

		if (!init_swscale(data, context))
			return false;
	}

	return true;
}

static bool create_audio_stream(struct ffmpeg_data *data)
{
	blog(LOG_INFO, "[CallStack] create_audio_stream(data=%p)", data);
	AVCodecContext *context;
	struct obs_audio_info aoi;

	if (!obs_get_audio_info(&aoi)) {
		blog(LOG_WARNING, "No active audio");
		return false;
	}

	if (!new_stream(data, &data->audio, &data->acodec,
				data->output->oformat->audio_codec,
				data->config.audio_encoder))
		return false;

	context              = data->audio->codec;
	context->bit_rate    = data->config.audio_bitrate * 1000;
	context->time_base   = (AVRational){ 1, aoi.samples_per_sec };
	context->channels    = get_audio_channels(aoi.speakers);
	context->sample_rate = aoi.samples_per_sec;
	context->channel_layout =
			av_get_default_channel_layout(context->channels);

	//AVlib default channel layout for 5 channels is 5.0 ; fix for 4.1
	if (aoi.speakers == SPEAKERS_4POINT1)
		context->channel_layout = av_get_channel_layout("4.1");

	context->sample_fmt  = data->acodec->sample_fmts ?
		data->acodec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;

	data->audio->time_base = context->time_base;

	data->audio_samplerate = aoi.samples_per_sec;
	data->audio_format = convert_ffmpeg_sample_format(context->sample_fmt);
	data->audio_planes = get_audio_planes(data->audio_format, aoi.speakers);
	data->audio_size = get_audio_size(data->audio_format, aoi.speakers, 1);

	if (data->output->oformat->flags & AVFMT_GLOBALHEADER)
		context->flags |= CODEC_FLAG_GLOBAL_H;

	return open_audio_codec(data);
}

static inline bool init_streams(struct ffmpeg_data *data)
{
	blog(LOG_INFO, "[CallStack] init_streams(data=%p)", data);
	AVOutputFormat *format = data->output->oformat;

	if (format->video_codec != AV_CODEC_ID_NONE)
		if (!create_video_stream(data))
			return false;

	if (format->audio_codec != AV_CODEC_ID_NONE)
		if (!create_audio_stream(data))
			return false;

	return true;
}

static inline bool open_output_file(struct ffmpeg_data *data)
{
	blog(LOG_INFO, "[CallStack] open_output_file(data=%p)", data);
	AVOutputFormat *format = data->output->oformat;
	int ret;

	AVDictionary *dict = NULL;
    blog(LOG_INFO, "[ProcessDetail] muxer_settings=%s", data->config.muxer_settings);
	if ((ret = av_dict_parse_string(&dict, data->config.muxer_settings,
				"=", "|", 0))) {
		blog(LOG_WARNING, "Failed to parse muxer settings: %s\n%s",
				av_err2str(ret), data->config.muxer_settings);

		av_dict_free(&dict);
		return false;
	}

	if (av_dict_count(dict) > 0) {
		struct dstr str = {0};

		AVDictionaryEntry *entry = NULL;
		while ((entry = av_dict_get(dict, "", entry,
						AV_DICT_IGNORE_SUFFIX)))
			dstr_catf(&str, "\n\t%s=%s", entry->key, entry->value);

		blog(LOG_INFO, "[ProcessDetail] Using muxer settings: %s", str.array);
		dstr_free(&str);
	}

	if ((format->flags & AVFMT_NOFILE) == 0) {
		ret = avio_open2(&data->output->pb, data->config.url,
				AVIO_FLAG_WRITE, NULL, &dict);
		if (ret < 0) {
			blog(LOG_WARNING, "Couldn't open '%s', %s",
					data->config.url, av_err2str(ret));
			av_dict_free(&dict);
			return false;
		}
	}

	strncpy(data->output->filename, data->config.url,
			sizeof(data->output->filename));
	data->output->filename[sizeof(data->output->filename) - 1] = 0;

	ret = avformat_write_header(data->output, &dict);
	if (ret < 0) {
		blog(LOG_WARNING, "Error opening '%s': %s",
				data->config.url, av_err2str(ret));
		return false;
	}

	if (av_dict_count(dict) > 0) {
		struct dstr str = {0};

		AVDictionaryEntry *entry = NULL;
		while ((entry = av_dict_get(dict, "", entry,
						AV_DICT_IGNORE_SUFFIX)))
			dstr_catf(&str, "\n\t%s=%s", entry->key, entry->value);

		blog(LOG_INFO, "Invalid muxer settings: %s", str.array);
		dstr_free(&str);
	}

	av_dict_free(&dict);

	return true;
}

static inline const char *safe_str(const char *s)
{
	if (s == NULL)
		return "(NULL)";
	else
		return s;
}

static bool ffmpeg_data_init(struct ffmpeg_data *data, struct ffmpeg_cfg *config)
{
	blog(LOG_INFO, "[CallStack] ffmpeg_data_init(data=%p, config=%p)", data, config);

	memset(data, 0, sizeof(struct ffmpeg_data));
	data->config = *config;

	if (!config->url || !*config->url)
		return false;

	av_register_all();
	avformat_network_init();

	AVOutputFormat *output_format = av_guess_format("hls", data->config.url, NULL);

	if (output_format == NULL) {
		blog(LOG_WARNING, "Couldn't find matching output format with "
				" parameters: name=%s, url=%s, mime=%s",
				safe_str(data->config.format_name),
				safe_str(data->config.url),
				safe_str(data->config.format_mime_type));
		goto fail;
	}
    //blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>dacast_format:%s %s", output_format->long_name, output_format->mime_type);

	avformat_alloc_output_context2(&data->output, output_format, NULL, NULL);

    if (data->config.format_name)
        set_encoder_ids(data);

	if (!data->output) {
		blog(LOG_WARNING, "Couldn't create avformat context");
		goto fail;
	}

	if (!init_streams(data))
		goto fail;
	if (!open_output_file(data))
		goto fail;

	av_dump_format(data->output, 0, NULL, 1);

	data->initialized = true;
	return true;

fail:
	blog(LOG_WARNING, "ffmpeg_data_init failed");
	ffmpeg_data_free(data);
	return false;
}


static void encode_audio(struct dacast_hls_output *output,
		struct AVCodecContext *context, size_t block_size)
{
	//blog(LOG_INFO, "[CallStack] encode_audio(output=%p, context=%p, block_size=%d)", output, context, block_size);
	struct ffmpeg_data *data = &output->ff_data;

	AVPacket packet = {0};
	int ret, got_packet;
	size_t total_size = data->frame_size * block_size * context->channels;

	data->aframe->nb_samples = data->frame_size;
	data->aframe->pts = av_rescale_q(data->total_samples,
			(AVRational){1, context->sample_rate},
			context->time_base);

	ret = avcodec_fill_audio_frame(data->aframe, context->channels,
			context->sample_fmt, data->samples[0],
			(int)total_size, 1);
	if (ret < 0) {
		blog(LOG_WARNING, "encode_audio: avcodec_fill_audio_frame "
		                  "failed: %s", av_err2str(ret));
		return;
	}

	data->total_samples += data->frame_size;

#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(57, 40, 101)
	ret = avcodec_send_frame(context, data->aframe);
	if (ret == 0)
		ret = avcodec_receive_packet(context, &packet);

	got_packet = (ret == 0);

	if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
		ret = 0;
#else
	ret = avcodec_encode_audio2(context, &packet, data->aframe,
			&got_packet);
#endif
	if (ret < 0) {
		blog(LOG_WARNING, "encode_audio: Error encoding audio: %s",
				av_err2str(ret));
		return;
	}

	if (!got_packet)
		return;

	packet.pts = rescale_ts(packet.pts, context, data->audio->time_base);
	packet.dts = rescale_ts(packet.dts, context, data->audio->time_base);
	packet.duration = (int)av_rescale_q(packet.duration, context->time_base,
			data->audio->time_base);
	packet.stream_index = data->audio->index;

    //blog(LOG_INFO, "in pts: %" PRId64 " dts: %" PRId64, packet.pts, packet.dts);

	pthread_mutex_lock(&output->write_mutex);
	da_push_back(output->packets, &packet);
	pthread_mutex_unlock(&output->write_mutex);
	os_sem_post(output->write_sem);
}


static bool prepare_audio(struct ffmpeg_data *data,
		const struct audio_data *frame, struct audio_data *output)
{
	//blog(LOG_INFO, "[CallStack] prepare_audio(data=%p, frame=%p, output=%p)", data, frame, output);
	*output = *frame;

	if (frame->timestamp < data->start_timestamp) {
		uint64_t duration = (uint64_t)frame->frames * 1000000000 /
			(uint64_t)data->audio_samplerate;
		uint64_t end_ts = (frame->timestamp + duration);
		uint64_t cutoff;

		if (end_ts <= data->start_timestamp)
			return false;

		cutoff = data->start_timestamp - frame->timestamp;
		output->timestamp += cutoff;

		cutoff = cutoff * (uint64_t)data->audio_samplerate /
			1000000000;

		for (size_t i = 0; i < data->audio_planes; i++)
			output->data[i] += data->audio_size * (uint32_t)cutoff;
		output->frames -= (uint32_t)cutoff;
	}

	return true;
}


static void receive_audio(void *param, struct audio_data *frame)
{
	//blog(LOG_INFO, "[CallStack] receive_audio(param=%p, frame=%p)", param, frame);
	struct dacast_hls_output *output = param;
	struct ffmpeg_data   *data   = &output->ff_data;
	size_t frame_size_bytes;
	struct audio_data in;

	// codec doesn't support audio or none configured
	if (!data->audio)
		return;

	AVCodecContext *context = data->audio->codec;

	if (!data->start_timestamp)
		return;
	if (!prepare_audio(data, frame, &in))
		return;

	if (!output->audio_start_ts)
		output->audio_start_ts = in.timestamp;

	frame_size_bytes = (size_t)data->frame_size * data->audio_size;

	for (size_t i = 0; i < data->audio_planes; i++)
		circlebuf_push_back(&data->excess_frames[i], in.data[i],
				in.frames * data->audio_size);

	while (data->excess_frames[0].size >= frame_size_bytes) {
		for (size_t i = 0; i < data->audio_planes; i++)
			circlebuf_pop_front(&data->excess_frames[i],
					data->samples[i], frame_size_bytes);

		encode_audio(output, context, data->audio_size);
	}
}


static inline void copy_data(AVFrame *pic, const struct video_data *frame,
		int height, enum AVPixelFormat format)
{
	//blog(LOG_INFO, "[CallStack] copy_data(pic=%p, frame=%p, height=%d, format=%d)", pic, frame, height, format);
	int h_chroma_shift, v_chroma_shift;
	av_pix_fmt_get_chroma_sub_sample(format, &h_chroma_shift, &v_chroma_shift);
	for (int plane = 0; plane < MAX_AV_PLANES; plane++) {
		if (!frame->data[plane])
			continue;

		int frame_rowsize = (int)frame->linesize[plane];
		int pic_rowsize   = pic->linesize[plane];
		int bytes = frame_rowsize < pic_rowsize ?
			frame_rowsize : pic_rowsize;
		int plane_height = height >> (plane ? v_chroma_shift : 0);

		for (int y = 0; y < plane_height; y++) {
			int pos_frame = y * frame_rowsize;
			int pos_pic   = y * pic_rowsize;

			memcpy(pic->data[plane] + pos_pic,
			       frame->data[plane] + pos_frame,
			       bytes);
		}
	}
}

static void receive_video(void *param, struct video_data *frame)
{
	//blog(LOG_INFO, "[CallStack] receive_video(param=%p, frame=%p)", param, frame);
	struct dacast_hls_output *output = param;
	struct ffmpeg_data   *data   = &output->ff_data;

	// codec doesn't support video or none configured
	if (!data->video)
		return;

	AVCodecContext *context = data->video->codec;
	AVPacket packet = {0};
	int ret = 0, got_packet;

	av_init_packet(&packet);

	if (!output->video_start_ts)
		output->video_start_ts = frame->timestamp;
	if (!data->start_timestamp)
		data->start_timestamp = frame->timestamp;

	if (!!data->swscale)
		sws_scale(data->swscale, (const uint8_t *const *)frame->data,
				(const int*)frame->linesize,
				0, data->config.height, data->vframe->data,
				data->vframe->linesize);
	else
		copy_data(data->vframe, frame, context->height, context->pix_fmt);
#if LIBAVFORMAT_VERSION_MAJOR < 58
	if (data->output->flags & AVFMT_RAWPICTURE) {
		packet.flags        |= AV_PKT_FLAG_KEY;
		packet.stream_index  = data->video->index;
		packet.data          = data->vframe->data[0];
		packet.size          = sizeof(AVPicture);

		pthread_mutex_lock(&output->write_mutex);
		da_push_back(output->packets, &packet);
		pthread_mutex_unlock(&output->write_mutex);
		os_sem_post(output->write_sem);

	} else {
#endif
		data->vframe->pts = data->total_frames;
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(57, 40, 101)
		ret = avcodec_send_frame(context, data->vframe);
		if (ret == 0)
			ret = avcodec_receive_packet(context, &packet);

		got_packet = (ret == 0);

		if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
			ret = 0;
#else
		ret = avcodec_encode_video2(context, &packet, data->vframe,
				&got_packet);
#endif
		if (ret < 0) {
			blog(LOG_WARNING, "receive_video: Error encoding "
			                  "video: %s", av_err2str(ret));
			return;
		}

		if (!ret && got_packet && packet.size) {
	        // blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>got packet");

			packet.pts = rescale_ts(packet.pts, context,
					data->video->time_base);
			packet.dts = rescale_ts(packet.dts, context,
					data->video->time_base);
			packet.duration = (int)av_rescale_q(packet.duration,
					context->time_base,
					data->video->time_base);

			pthread_mutex_lock(&output->write_mutex);
			da_push_back(output->packets, &packet);
			pthread_mutex_unlock(&output->write_mutex);
			os_sem_post(output->write_sem);
		} else {
			ret = 0;
		}
#if LIBAVFORMAT_VERSION_MAJOR < 58
	}
#endif
	if (ret != 0) {
		blog(LOG_WARNING, "receive_video: Error writing video: %s",
				av_err2str(ret));
	}

	data->total_frames++;

}

static uint64_t dacast_hls_ffmpeg_output_total_bytes(void *data)
{
	//blog(LOG_INFO, "[CallStack] dacast_hls_ffmpeg_output_total_bytes(data=%p)", data);
	// blog(LOG_INFO, ">>>>>>>>>>>>>>>>>>dacast_output_total_bytes");
    //this is used for the bitrate display at the bottom right of OBS
	struct dacast_hls_output *output = data;
	return output->total_bytes;
}


static int dacast_hls_ffmpeg_output_dropped_frames(void *data)
{
	//blog(LOG_INFO, "[CallStack] dacast_hls_ffmpeg_output_dropped_frames(data=%p)", data);
	struct dacast_hls_output *output = data;
	return output->dropped_frames;
}


struct obs_output_info dacast_hls_ffmpeg_output = {
    .id        = "dacast_hls_ffmpeg_output",
    .flags     = OBS_OUTPUT_AV |
                OBS_OUTPUT_SERVICE |
                OBS_OUTPUT_MULTI_TRACK,
    .encoded_video_codecs = "h264",
    .encoded_audio_codecs = "aac",
    .get_name  = dacast_hls_ffmpeg_output_getname,
    .create    = dacast_hls_ffmpeg_output_create,
    .destroy   = dacast_hls_ffmpeg_output_destroy,
    .start     = dacast_hls_ffmpeg_output_start,
    .stop      = dacast_hls_ffmpeg_output_stop,
    .raw_video = receive_video,
    .raw_audio = receive_audio,
    .get_total_bytes = dacast_hls_ffmpeg_output_total_bytes,
    .get_dropped_frames = dacast_hls_ffmpeg_output_dropped_frames
};
