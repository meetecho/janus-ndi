/*
 * Author:  Lorenzo Miniero <lorenzo@meetecho.com>
 * License: GNU General Public License v3
 */

#include "plugins/plugin.h"

#include <sys/time.h>
#include <jansson.h>
#include <curl/curl.h>

#include <Processing.NDI.Lib.h>
#include <opus/opus.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include "debug.h"
#include "apierror.h"
#include "config.h"
#include "mutex.h"
#include "rtp.h"
#include "rtcp.h"
#include "sdp-utils.h"
#include "utils.h"

/* The PNG test pattern as a header file */
#include "pattern.h"

/* VP8 stuff */
#if defined(__ppc__) || defined(__ppc64__)
	# define swap2(d)  \
	((d&0x000000ff)<<8) |  \
	((d&0x0000ff00)>>8)
#else
	# define swap2(d) d
#endif

/* Plugin information */
#define JANUS_NDI_VERSION			4
#define JANUS_NDI_VERSION_STRING	"0.0.4"
#define JANUS_NDI_DESCRIPTION		"This plugin acts as a gateway between WebRTC and NDI."
#define JANUS_NDI_NAME				"JANUS NDI plugin"
#define JANUS_NDI_AUTHOR			"Meetecho s.r.l."
#define JANUS_NDI_PACKAGE			"janus.plugin.ndi"

/* Plugin methods */
janus_plugin *create(void);
int janus_ndi_init(janus_callbacks *callback, const char *config_path);
void janus_ndi_destroy(void);
int janus_ndi_get_api_compatibility(void);
int janus_ndi_get_version(void);
const char *janus_ndi_get_version_string(void);
const char *janus_ndi_get_description(void);
const char *janus_ndi_get_name(void);
const char *janus_ndi_get_author(void);
const char *janus_ndi_get_package(void);
void janus_ndi_create_session(janus_plugin_session *handle, int *error);
struct janus_plugin_result *janus_ndi_handle_message(janus_plugin_session *handle, char *transaction, json_t *message, json_t *jsep);
json_t *janus_ndi_handle_admin_message(json_t *message);
void janus_ndi_setup_media(janus_plugin_session *handle);
void janus_ndi_incoming_rtp(janus_plugin_session *handle, janus_plugin_rtp *packet);
void janus_ndi_incoming_rtcp(janus_plugin_session *handle, janus_plugin_rtcp *packet);
void janus_ndi_hangup_media(janus_plugin_session *handle);
static void janus_ndi_hangup_media_internal(janus_plugin_session *handle);
void janus_ndi_destroy_session(janus_plugin_session *handle, int *error);
json_t *janus_ndi_query_session(janus_plugin_session *handle);

/* Plugin setup */
static janus_plugin janus_ndi_plugin =
	JANUS_PLUGIN_INIT (
		.init = janus_ndi_init,
		.destroy = janus_ndi_destroy,

		.get_api_compatibility = janus_ndi_get_api_compatibility,
		.get_version = janus_ndi_get_version,
		.get_version_string = janus_ndi_get_version_string,
		.get_description = janus_ndi_get_description,
		.get_name = janus_ndi_get_name,
		.get_author = janus_ndi_get_author,
		.get_package = janus_ndi_get_package,

		.create_session = janus_ndi_create_session,
		.handle_message = janus_ndi_handle_message,
		.handle_admin_message = janus_ndi_handle_admin_message,
		.setup_media = janus_ndi_setup_media,
		.incoming_rtp = janus_ndi_incoming_rtp,
		.incoming_rtcp = janus_ndi_incoming_rtcp,
		.hangup_media = janus_ndi_hangup_media,
		.destroy_session = janus_ndi_destroy_session,
		.query_session = janus_ndi_query_session,
	);

/* Plugin creator */
janus_plugin *create(void) {
	JANUS_LOG(LOG_VERB, "%s created!\n", JANUS_NDI_NAME);
	return &janus_ndi_plugin;
}

/* Parameter validation */
static struct janus_json_parameter request_parameters[] = {
	{"request", JSON_STRING, JANUS_JSON_PARAM_REQUIRED}
};
static struct janus_json_parameter create_parameters[] = {
	{"name", JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"metadata", JSON_STRING, 0},
	{"placeholder", JSON_STRING, 0},
	{"width", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"height", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"keep_ratio", JANUS_JSON_BOOL, 0}
};
static struct janus_json_parameter updateimg_parameters[] = {
	{"name", JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"placeholder", JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"width", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"height", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"keep_ratio", JANUS_JSON_BOOL, 0}
};
static struct janus_json_parameter destroy_parameters[] = {
	{"name", JSON_STRING, JANUS_JSON_PARAM_REQUIRED}
};
static struct janus_json_parameter translate_parameters[] = {
	{"name", JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"metadata", JSON_STRING, 0},
	{"videocodec", JSON_STRING, 0},
	{"bitrate", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"width", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"height", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"fps", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"ondisconnect", JSON_OBJECT, 0},
	{"audio", JANUS_JSON_BOOL, 0},
	{"video", JANUS_JSON_BOOL, 0},
	{"strict", JANUS_JSON_BOOL, 0},
};
static struct janus_json_parameter ondisconnect_parameters[] = {
	{"image", JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"color", JSON_STRING, 0},
};
static struct janus_json_parameter configure_parameters[] = {
	{"bitrate", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE},
	{"keyframe", JANUS_JSON_BOOL, 0},
	{"paused", JANUS_JSON_BOOL, 0},
	{"audio", JANUS_JSON_BOOL, 0},
	{"video", JANUS_JSON_BOOL, 0},
};

/* Useful stuff */
static volatile gint initialized = 0, stopping = 0;
static gboolean notify_events = TRUE;
static janus_callbacks *gateway = NULL;

static GThread *handler_thread;
static void *janus_ndi_handler(void *data);

/* Default buffer size in ms */
static int64_t buffer_size = 200000;
/* Test pattern stuff */
static AVFrame *test_pattern = NULL;
static const char *test_pattern_name = "janus-ndi-test";
static GThread *test_pattern_thread = NULL;
static volatile gint test_pattern_running = 0;
static void *janus_ndi_send_test_pattern(void *data);

/* Images management (for placeholders) */
static GHashTable *images = NULL;
static janus_mutex img_mutex;
static AVFrame *janus_ndi_download_image(const char *path);
static AVFrame *janus_ndi_decode_image(char *filename);
static void janus_ndi_avframe_free(AVFrame *frame);
static AVFrame *janus_ndi_blit_frameYUV(AVFrame *dst, AVFrame *src,
	int fromX, int fromY, int fromW, int fromH, int toX, int toY, enum AVPixelFormat pix_fmt);
static AVFrame *janus_ndi_generate_disconnected_image(const char *path,
	const char *color, int width, int height);

/* Buffered audio/video packet */
typedef struct janus_ndi_buffer_packet {
	char *buffer;			/* Pointer to the packet data, if RTP */
	int len;				/* Size of the packet */
	uint32_t timestamp;		/* RTP timestamp of the packet */
	uint16_t seq_number;	/* RTP sequence number of the packet */
	int64_t inserted;		/* Monotonic insert time */
} janus_ndi_buffer_packet;
static janus_ndi_buffer_packet *janus_ndi_buffer_packet_create(char *buffer, int len) {
	janus_ndi_buffer_packet *pkt = g_malloc(sizeof(janus_ndi_buffer_packet));
	pkt->buffer = g_malloc(len);
	pkt->len = len;
	memcpy(pkt->buffer, buffer, len);
	janus_rtp_header *rtp = (janus_rtp_header *)buffer;
	pkt->timestamp = ntohl(rtp->timestamp);
	pkt->seq_number = ntohs(rtp->seq_number);
	pkt->inserted = g_get_monotonic_time();
	return pkt;
}
static void janus_ndi_buffer_packet_destroy(janus_ndi_buffer_packet *pkt) {
	if(!pkt)
		return;
	g_free(pkt->buffer);
	g_free(pkt);
}
static gint janus_ndi_buffer_packet_compare(gconstpointer a, gconstpointer b, gpointer user_data) {
	janus_ndi_buffer_packet *bpa = (janus_ndi_buffer_packet *)a;
	janus_ndi_buffer_packet *bpb = (janus_ndi_buffer_packet *)b;
	if(bpa->timestamp == bpb->timestamp) {
		/* Check the sequence numbers */
		if(bpa->seq_number == bpb->seq_number) {
			/* Same packet? */
			return 0;
		} else if(bpa->seq_number < bpb->seq_number) {
			if(bpb->seq_number - bpa->seq_number < 30000) {
				/* Sequence number wrapped */
				return -1;
			} else {
				/* Regular ordering */
				return 1;
			}
		} else if(bpa->seq_number > bpb->seq_number) {
			if(bpa->seq_number - bpb->seq_number < 30000) {
				/* Sequence number wrapped */
				return 1;
			} else {
				/* Regular ordering */
				return -1;
			}
		}
	} else if(bpa->timestamp < bpb->timestamp) {
		if(bpb->timestamp - bpa->timestamp < 2*1000*1000*1000) {
			/* Timestamp wrapped */
			return -1;
		} else {
			/* Regular ordering */
			return 1;
		}
	} else if(bpa->timestamp > bpb->timestamp) {
		if(bpa->timestamp - bpb->timestamp < 2*1000*1000*1000) {
			/* Timestamp wrapped */
			return 1;
		} else {
			/* Regular ordering */
			return -1;
		}
	}
	return 0;
}

/* Message from the core to the plugin, to process asynchronously */
typedef struct janus_ndi_message {
	janus_plugin_session *handle;
	char *transaction;
	json_t *message;
	json_t *jsep;
} janus_ndi_message;
static GAsyncQueue *messages = NULL;
static janus_ndi_message exit_message;

/* NDI sender */
typedef struct janus_ndi_sender {
	char *name;								/* NDI name */
	char *metadata;							/* NDI metadata */
	NDIlib_send_instance_t instance;		/* NDI audio/video sender */
	gboolean placeholder;					/* Whether this sender will be shared or is owned */
	AVFrame *image;							/* Placeholder image to use, if required */
	/* Placeholder thread, if required */
	GThread *thread;
	/* Activity on the sender */
	gint64 last_updated;
	gboolean busy;
	/* Struct info */
	volatile gint destroyed;
	janus_refcount ref;
	janus_mutex mutex;
} janus_ndi_sender;
static void janus_ndi_sender_destroy(janus_ndi_sender *sender) {
	if(sender && g_atomic_int_compare_and_exchange(&sender->destroyed, 0, 1))
		janus_refcount_decrease(&sender->ref);
}
static void janus_ndi_sender_free(const janus_refcount *sender_ref) {
	janus_ndi_sender *sender = janus_refcount_containerof(sender_ref, janus_ndi_sender, ref);
	/* Also notify event handlers */
	if(notify_events && gateway->events_is_enabled()) {
		json_t *info = json_object();
		json_object_set_new(info, "name", json_string(sender->name));
		json_object_set_new(info, "event", json_string("destroyed"));
		gateway->notify_event(&janus_ndi_plugin, NULL, info);
	}
	/* This sender can be destroyed, free all the resources */
	JANUS_LOG(LOG_INFO, "[%s] Freeing NDI sender\n", sender->name);
	g_free(sender->name);
	if(sender->instance)
		NDIlib_send_destroy(sender->instance);
	g_free(sender->metadata);
	if(sender->image != NULL) {
		av_free(sender->image->data[0]);
		av_frame_free(&sender->image);
	}
	/* Done */
	g_free(sender);
}
static int janus_ndi_generate_placeholder_image(janus_ndi_sender *sender,
	const char *path, int width, int height, gboolean keep_ratio,
	int *error_code, char *error_cause, size_t error_cause_len);

/* User session */
typedef struct janus_ndi_session {
	janus_plugin_session *handle;
	/* SDP/RTP */
	gint64 sdp_version;
	janus_sdp *sdp;							/* The SDP this user sent */
#if (JANUS_PLUGIN_API_VERSION < 100)
	janus_rtp_switching_context rtpctx;		/* RTP context */
#else
	janus_rtp_switching_context artpctx, vrtpctx;	/* RTP contexts */
#endif
	uint16_t a_max_seq_nr, v_max_seq_nr;	/* Max sequence numbers */
	uint32_t bitrate;						/* Bitrate to enforce via REMB */
	/* NDI and audio/video decoders */
	OpusDecoder *audiodec;					/* Opus decoder */
	janus_videocodec vcodec;				/* Video codec */
	AVCodecContext *ctx;					/* Video decoder */
	gboolean strict_decoder;				/* Whether we should discard frames with missing packets */
	int width, height, fps;					/* Video width/height, and advertised FPS */
	int target_width, target_height;		/* Video width/height to scale to, if needed */
	char *ndi_name;							/* NDI name */
	janus_ndi_sender *ndi_sender;			/* NDI audio/video sender */
	gboolean external_sender;				/* Whether this session owns the NDI sender or not */
	char *ndi_metadata;						/* NDI metadata, if any */
	/* Queues */
	GQueue *audio_buffered_packets, *video_buffered_packets;
	/* Path to disconnected image and background color, if any */
	char *disconnected, *disconnected_color;
	/* Translation thread */
	GThread *thread;
	/* Struct info */
	volatile gint audio, video;
	volatile gint paused;
	volatile gint hangingup;
	volatile gint hangup;
	volatile gint destroyed;
	janus_refcount ref;
	janus_mutex mutex;
} janus_ndi_session;
static GHashTable *sessions;
static GHashTable *ndi_names;
static janus_mutex sessions_mutex = JANUS_MUTEX_INITIALIZER;

static void janus_ndi_session_destroy(janus_ndi_session *session) {
	if(session && g_atomic_int_compare_and_exchange(&session->destroyed, 0, 1))
		janus_refcount_decrease(&session->ref);
}
static void janus_ndi_session_free(const janus_refcount *session_ref) {
	janus_ndi_session *session = janus_refcount_containerof(session_ref, janus_ndi_session, ref);
	/* Remove the reference to the core plugin session */
	janus_refcount_decrease(&session->handle->ref);
	/* This session can be destroyed, free all the resources */
	janus_sdp_destroy(session->sdp);
	session->sdp = NULL;
	if(session->audiodec)
		opus_decoder_destroy(session->audiodec);
	if(session->ctx) {
		avcodec_free_context(&session->ctx);
		av_free(session->ctx);
	}
	g_free(session->ndi_name);
	g_free(session->ndi_metadata);
	g_free(session->disconnected);
	g_free(session->disconnected_color);
	if(session->audio_buffered_packets)
		g_queue_free_full(session->audio_buffered_packets, (GDestroyNotify)janus_ndi_buffer_packet_destroy);
	if(session->video_buffered_packets)
		g_queue_free_full(session->video_buffered_packets, (GDestroyNotify)janus_ndi_buffer_packet_destroy);
	/* Done */
	g_free(session);
	session = NULL;
}

static void janus_ndi_message_free(janus_ndi_message *msg) {
	if(!msg || msg == &exit_message)
		return;

	if(msg->handle && msg->handle->plugin_handle) {
		janus_ndi_session *session = (janus_ndi_session *)msg->handle->plugin_handle;
		janus_refcount_decrease(&session->ref);
	}
	msg->handle = NULL;

	g_free(msg->transaction);
	msg->transaction = NULL;
	if(msg->message)
		json_decref(msg->message);
	msg->message = NULL;
	if(msg->jsep)
		json_decref(msg->jsep);
	msg->jsep = NULL;

	g_free(msg);
}

/* Helper to check if an RTP packet is out of order */
static gboolean janus_ndi_rtp_is_outoforder(janus_ndi_session *session, janus_rtp_header *header, gboolean video) {
	if(header == NULL || session == NULL)
		return FALSE;
	uint16_t seq = ntohs(header->seq_number);
	uint16_t max_seq_nr = (video ? session->v_max_seq_nr : session->a_max_seq_nr);
	if((int16_t)(seq - max_seq_nr) > 0) {
		/* Packet is in order, update max_seq_nr */
		if(video)
			session->v_max_seq_nr = seq;
		else
			session->a_max_seq_nr = seq;
		return FALSE;
	} else {
		/* Packet is out of order */
		JANUS_LOG(LOG_WARN, "Out of order packet (%"SCNu16", expecting %"SCNu16")\n",
			seq, (max_seq_nr+1));
		return TRUE;
	}
}

/* NDI placeholder thread, if required */
static void *janus_ndi_placeholder_thread(void *data);
/* Audio/video processing thread */
static void *janus_ndi_processing_thread(void *data);

/* Error codes */
#define JANUS_NDI_ERROR_UNKNOWN_ERROR		499
#define JANUS_NDI_ERROR_NO_MESSAGE			440
#define JANUS_NDI_ERROR_INVALID_JSON		441
#define JANUS_NDI_ERROR_INVALID_REQUEST		442
#define JANUS_NDI_ERROR_MISSING_ELEMENT		443
#define JANUS_NDI_ERROR_INVALID_ELEMENT		444
#define JANUS_NDI_ERROR_WRONG_STATE			445
#define JANUS_NDI_ERROR_MISSING_SDP			446
#define JANUS_NDI_ERROR_INVALID_SDP			447
#define JANUS_NDI_ERROR_CODEC_ERROR			448
#define JANUS_NDI_ERROR_NDI_ERROR			449
#define JANUS_NDI_ERROR_NDI_NAME_IN_USE		450
#define JANUS_NDI_ERROR_NDI_NAME_NOT_FOUND	451
#define JANUS_NDI_ERROR_IMAGE				452
#define JANUS_NDI_ERROR_THREAD				453


/* Plugin implementation */
int janus_ndi_init(janus_callbacks *callback, const char *config_path) {
	if(g_atomic_int_get(&stopping)) {
		/* Still stopping from before */
		return -1;
	}
	if(callback == NULL || config_path == NULL) {
		/* Invalid arguments */
		return -1;
	}

	/* Initialize NDI */
	if(!NDIlib_initialize()) {
		/* Error initializing the NDI library */
		JANUS_LOG(LOG_FATAL, "Error initializing NDI library...\n");
		return -1;
	}
	/* FFmpeg initialization */
#if ( LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58,9,100) )
	av_register_all();
#endif

	/* Read configuration */
	char filename[255];
	g_snprintf(filename, 255, "%s/%s.jcfg", config_path, JANUS_NDI_PACKAGE);
	JANUS_LOG(LOG_VERB, "Configuration file: %s\n", filename);
	janus_config *config = janus_config_parse(filename);
	if(config == NULL) {
		JANUS_LOG(LOG_WARN, "Couldn't find .jcfg configuration file (%s), trying .cfg\n", JANUS_NDI_PACKAGE);
		g_snprintf(filename, 255, "%s/%s.cfg", config_path, JANUS_NDI_PACKAGE);
		JANUS_LOG(LOG_VERB, "Configuration file: %s\n", filename);
		config = janus_config_parse(filename);
	}
	if(config != NULL) {
		janus_config_print(config);

		janus_config_category *config_general = janus_config_get_create(config, NULL, janus_config_type_category, "general");
		/* Check if we need to enforce a custom buffer size */
		janus_config_item *item = janus_config_get(config, config_general, janus_config_type_item, "buffer_size");
		if(item && item->value) {
			/* Enforce buffer size */
			int bs = atoi(item->value);
			if(bs < 0) {
				JANUS_LOG(LOG_WARN, "Invalid buffer size %s, falling back to %"SCNi64"\n", item->value, buffer_size);
			} else {
				buffer_size = bs*1000;
				JANUS_LOG(LOG_INFO, "Setting buffer size to %dms\n", bs);
			}
		}
		item = janus_config_get(config, config_general, janus_config_type_item, "events");
		if(item != NULL && item->value != NULL)
			notify_events = janus_is_true(item->value);
		if(!notify_events && callback->events_is_enabled()) {
			JANUS_LOG(LOG_WARN, "Notification of events to handlers disabled for %s\n", JANUS_NDI_NAME);
		}
		/* Done */
		janus_config_destroy(config);
	}
	config = NULL;

	/* Load test pattern */
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_PNG);
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	int err = avcodec_open2(ctx, codec, NULL);
	if(err < 0) {
		JANUS_LOG(LOG_ERR, "Couldn't initiate the PNG decoder... %d (%s)\n", err, av_err2str(err));
		avcodec_free_context(&ctx);
		return -1;
	}
	AVPacket packet = { 0 };
	packet.data = (uint8_t *)pattern_png.data;
	packet.size = pattern_png.size;
	AVFrame *bgRGB = av_frame_alloc();
	err = avcodec_send_packet(ctx, &packet);
	if(err < 0) {
		JANUS_LOG(LOG_ERR, "Error decoding test pattern image: %d (%s)\n", err, av_err2str(err));
		avcodec_free_context(&ctx);
		av_frame_free(&bgRGB);
		av_packet_unref(&packet);
		return -1;
	}
	err = avcodec_receive_frame(ctx, bgRGB);
	if(err < 0) {
		JANUS_LOG(LOG_ERR, "Error decoding test pattern image: %d (%s)\n", err, av_err2str(err));
		avcodec_free_context(&ctx);
		av_frame_free(&bgRGB);
		av_packet_unref(&packet);
		return -1;
	}
	JANUS_LOG(LOG_INFO, "Test pattern frame loaded: %dx%d, %s\n",
		bgRGB->width, bgRGB->height, av_get_pix_fmt_name(bgRGB->format));
	av_packet_unref(&packet);
	avcodec_free_context(&ctx);
	/* Convert the test frame pattern to the format NDI expexts */
	struct SwsContext *sws = sws_getContext(bgRGB->width, bgRGB->height, bgRGB->format,
		bgRGB->width, bgRGB->height, AV_PIX_FMT_UYVY422, SWS_FAST_BILINEAR, NULL, NULL, NULL);
	if(sws == NULL) {
		JANUS_LOG(LOG_ERR, "Couldn't initialize UYVY422 scaler...\n");
		av_frame_free(&bgRGB);
		return -1;
	}
	test_pattern = av_frame_alloc();
	test_pattern->width = bgRGB->width;
	test_pattern->height = bgRGB->height;
	test_pattern->format = AV_PIX_FMT_UYVY422;
	err = av_image_alloc(test_pattern->data, test_pattern->linesize,
		test_pattern->width, test_pattern->height, AV_PIX_FMT_UYVY422, 1);
	if(err < 0) {
		JANUS_LOG(LOG_ERR, "Error allocating test pattern frame: %d (%s)\n", err, av_err2str(err));
		av_frame_free(&bgRGB);
		return -1;
	}
	sws_scale(sws, (const uint8_t * const*)bgRGB->data, bgRGB->linesize,
		0, bgRGB->height, test_pattern->data, test_pattern->linesize);
	av_frame_free(&bgRGB);
	sws_freeContext(sws);
	JANUS_LOG(LOG_INFO, "Test pattern frame converted to NDI format: %dx%d, %s\n",
		test_pattern->width, test_pattern->height, av_get_pix_fmt_name(test_pattern->format));


	sessions = g_hash_table_new_full(NULL, NULL, NULL, (GDestroyNotify)janus_ndi_session_destroy);
	ndi_names = g_hash_table_new_full(g_str_hash, g_str_equal, (GDestroyNotify)g_free, (GDestroyNotify)janus_ndi_sender_destroy);
	messages = g_async_queue_new_full((GDestroyNotify) janus_ndi_message_free);
	/* Static images management */
	images = g_hash_table_new_full(g_str_hash, g_str_equal,
		(GDestroyNotify)g_free, (GDestroyNotify)janus_ndi_avframe_free);
	janus_mutex_init(&img_mutex);
	/* This is the callback we'll need to invoke to contact the Janus core */
	gateway = callback;

	g_atomic_int_set(&initialized, 1);

	GError *error = NULL;
	/* Launch the thread that will handle incoming messages */
	handler_thread = g_thread_try_new("ndi handler", janus_ndi_handler, NULL, &error);
	if(error != NULL) {
		g_atomic_int_set(&initialized, 0);
		JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the NDI handler thread...\n",
			error->code, error->message ? error->message : "??");
		g_error_free(error);
		return -1;
	}
	JANUS_LOG(LOG_INFO, "%s initialized!\n", JANUS_NDI_NAME);

	return 0;
}

void janus_ndi_destroy(void) {
	if(!g_atomic_int_get(&initialized))
		return;
	g_atomic_int_set(&stopping, 1);

	g_async_queue_push(messages, &exit_message);
	if(handler_thread != NULL) {
		g_thread_join(handler_thread);
		handler_thread = NULL;
	}
	if(test_pattern_thread != NULL) {
		g_atomic_int_set(&test_pattern_running, -1);
		g_thread_join(test_pattern_thread);
		test_pattern_thread = NULL;
	}
	av_freep(&test_pattern->data[0]);
	test_pattern->data[0] = NULL;
	av_free(test_pattern);
	/* FIXME We should destroy the sessions cleanly */
	janus_mutex_lock(&sessions_mutex);
	g_hash_table_destroy(sessions);
	sessions = NULL;
	g_hash_table_destroy(ndi_names);
	ndi_names = NULL;
	janus_mutex_unlock(&sessions_mutex);
	g_async_queue_unref(messages);
	messages = NULL;
	g_atomic_int_set(&initialized, 0);
	g_atomic_int_set(&stopping, 0);

	/* Destroy the NDI stack */
	NDIlib_destroy();
	/* Get rid of static images */
	janus_mutex_lock(&img_mutex);
	g_hash_table_destroy(images);
	images = NULL;
	janus_mutex_unlock(&img_mutex);

	JANUS_LOG(LOG_INFO, "%s destroyed!\n", JANUS_NDI_NAME);
}

int janus_ndi_get_api_compatibility(void) {
	/* Important! This is what your plugin MUST always return: don't lie here or bad things will happen */
	return JANUS_PLUGIN_API_VERSION;
}

int janus_ndi_get_version(void) {
	return JANUS_NDI_VERSION;
}

const char *janus_ndi_get_version_string(void) {
	return JANUS_NDI_VERSION_STRING;
}

const char *janus_ndi_get_description(void) {
	return JANUS_NDI_DESCRIPTION;
}

const char *janus_ndi_get_name(void) {
	return JANUS_NDI_NAME;
}

const char *janus_ndi_get_author(void) {
	return JANUS_NDI_AUTHOR;
}

const char *janus_ndi_get_package(void) {
	return JANUS_NDI_PACKAGE;
}

static janus_ndi_session *janus_ndi_lookup_session(janus_plugin_session *handle) {
	janus_ndi_session *session = NULL;
	if (g_hash_table_contains(sessions, handle)) {
		session = (janus_ndi_session *)handle->plugin_handle;
	}
	return session;
}

void janus_ndi_create_session(janus_plugin_session *handle, int *error) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		*error = -1;
		return;
	}
	janus_ndi_session *session = g_malloc0(sizeof(janus_ndi_session));
	session->handle = handle;
	g_atomic_int_set(&session->destroyed, 0);
	g_atomic_int_set(&session->hangingup, 0);
	janus_mutex_init(&session->mutex);
	handle->plugin_handle = session;
	/* Done */
	janus_refcount_init(&session->ref, janus_ndi_session_free);

	janus_mutex_lock(&sessions_mutex);
	g_hash_table_insert(sessions, handle, session);
	janus_mutex_unlock(&sessions_mutex);

	return;
}

void janus_ndi_destroy_session(janus_plugin_session *handle, int *error) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		*error = -1;
		return;
	}
	janus_mutex_lock(&sessions_mutex);
	janus_ndi_session *session = janus_ndi_lookup_session(handle);
	if(!session) {
		janus_mutex_unlock(&sessions_mutex);
		JANUS_LOG(LOG_ERR, "No NDI session associated with this handle...\n");
		*error = -2;
		return;
	}
	JANUS_LOG(LOG_VERB, "Destroying NDI session (%s)...\n", session->ndi_name);
	janus_ndi_hangup_media_internal(handle);
	g_hash_table_remove(sessions, handle);
	janus_mutex_unlock(&sessions_mutex);
	return;
}

json_t *janus_ndi_query_session(janus_plugin_session *handle) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		return NULL;
	}
	janus_mutex_lock(&sessions_mutex);
	janus_ndi_session *session = janus_ndi_lookup_session(handle);
	if(!session) {
		janus_mutex_unlock(&sessions_mutex);
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return NULL;
	}
	janus_refcount_increase(&session->ref);
	janus_mutex_unlock(&sessions_mutex);
	/* Provide some generic info, e.g., if we're in a call and with whom */
	json_t *info = json_object();
	if(session->ndi_name) {
		json_object_set_new(info, "ndi-name", json_string(session->ndi_name));
		if(session->audiodec)
			json_object_set_new(info, "audio", json_true());
		if(session->ctx)
			json_object_set_new(info, "video", json_true());
		if(session->bitrate)
			json_object_set_new(info, "bitrate-cap", json_integer(session->bitrate));
		json_object_set_new(info, "paused", g_atomic_int_get(&session->paused) ? json_true() : json_false());
		json_object_set_new(info, "send-audio", g_atomic_int_get(&session->audio) ? json_true() : json_false());
		json_object_set_new(info, "send-video", g_atomic_int_get(&session->video) ? json_true() : json_false());
		json_object_set_new(info, "buffer-size", json_integer(buffer_size));
		if(session->ndi_sender) {
			json_object_set_new(info, "placeholder", session->ndi_sender->placeholder ? json_true() : json_false());
			json_object_set_new(info, "busy", session->ndi_sender->busy ? json_true() : json_false());
			json_object_set_new(info, "last-updated", json_integer(session->ndi_sender->last_updated));
		}
	}
	json_object_set_new(info, "hangingup", json_integer(g_atomic_int_get(&session->hangingup)));
	json_object_set_new(info, "destroyed", json_integer(g_atomic_int_get(&session->destroyed)));
	janus_refcount_decrease(&session->ref);
	return info;
}

/* Helper method to process synchronous requests */
static json_t *janus_ndi_process_synchronous_request(janus_ndi_session *session, json_t *message) {
	json_t *request = json_object_get(message, "request");
	const char *request_text = json_string_value(request);

	/* Parse the message */
	int error_code = 0;
	char error_cause[512];
	json_t *response = NULL;

	if(!strcasecmp(request_text, "create")) {
		JANUS_VALIDATE_JSON_OBJECT(message, create_parameters,
			error_code, error_cause, TRUE,
			JANUS_NDI_ERROR_MISSING_ELEMENT, JANUS_NDI_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto prepare_response;
		/* Pre-create an NDI sender with a placeholder image to send
		 * when there's no WebRTC PeerConnection currently feeding it */
		const char *name = json_string_value(json_object_get(message, "name"));
		if(!strcasecmp(name, test_pattern_name)) {
			/* This is a reserved name */
			JANUS_LOG(LOG_ERR, "This name cannot be used (reserved for test pattern)\n");
			error_code = JANUS_NDI_ERROR_NDI_NAME_IN_USE;
			g_snprintf(error_cause, 512, "This name cannot be used (reserved for test pattern)");
			goto prepare_response;
		}
		/* Make sure this name is not in use */
		janus_mutex_lock(&sessions_mutex);
		if(g_hash_table_lookup(ndi_names, name) != NULL) {
			/* Already in use */
			janus_mutex_unlock(&sessions_mutex);
			JANUS_LOG(LOG_ERR, "This name is already in use in the plugin\n");
			error_code = JANUS_NDI_ERROR_NDI_NAME_IN_USE;
			g_snprintf(error_cause, 512, "This name is already in use in the plugin");
			goto prepare_response;
		}
		/* Create a new sender */
		janus_ndi_sender *sender = g_malloc0(sizeof(janus_ndi_sender));
		janus_refcount_init(&sender->ref, janus_ndi_sender_free);
		janus_mutex_init(&sender->mutex);
		sender->name = g_strdup(name);
		sender->placeholder = TRUE;
		NDIlib_send_create_t NDI_send_create_desc = {0};
		NDI_send_create_desc.p_ndi_name = sender->name;
		sender->instance = NDIlib_send_create(&NDI_send_create_desc);
		if(sender->instance == NULL) {
			/* Error creating NDI source */
			janus_mutex_unlock(&sessions_mutex);
			JANUS_LOG(LOG_ERR, "Error creating NDI source for '%s'\n", name);
			janus_ndi_sender_destroy(sender);
			error_code = JANUS_NDI_ERROR_NDI_ERROR;
			g_snprintf(error_cause, 512, "Error creating NDI source for '%s'", name);
			goto prepare_response;
		}
		g_hash_table_insert(ndi_names, g_strdup(name), sender);
		/* Also check if we need to send some metadata */
		json_t *m = json_object_get(message, "metadata");
		if(m != NULL) {
			const char *metadata = json_string_value(m);
			g_free(sender->metadata);
			sender->metadata = metadata ? g_strdup(metadata) : NULL;
			NDIlib_send_clear_connection_metadata(sender->instance);
			NDIlib_metadata_frame_t NDI_product_type;
			NDI_product_type.p_data = sender->metadata;
			NDIlib_send_add_connection_metadata(sender->instance, &NDI_product_type);
		}
		/* Check if we're forcing a specific resolution for the placeholder */
		int width = -1, height = -1;
		json_t *w = json_object_get(message, "width");
		if(w != NULL) {
			width = json_integer_value(w);
			if(width <= 0 || width > 1920) {
				JANUS_LOG(LOG_WARN, "Invalid target width %d, sticking to actual placeholder resolution\n", width);
				width = -1;
			}
		}
		json_t *h = json_object_get(message, "height");
		if(h != NULL) {
			height = json_integer_value(h);
			if(height <= 0 || height > 1080) {
				JANUS_LOG(LOG_WARN, "Invalid target height %d, sticking to actual placeholder resolution\n", height);
				height = -1;
			}
		}
		json_t *kr = json_object_get(message, "keep_ratio");
		gboolean ratio = kr ? json_is_true(kr) : TRUE;
		/* Get the image to use as a placeholder */
		const char *placeholder_path = json_string_value(json_object_get(message, "placeholder"));
		if(janus_ndi_generate_placeholder_image(sender, placeholder_path, width, height, ratio,
				&error_code, error_cause, sizeof(error_cause)) < 0) {
			/* Error code and cause already set by the method */
			g_hash_table_remove(ndi_names, name);
			janus_mutex_unlock(&sessions_mutex);
			goto prepare_response;
		}
		/* Also notify event handlers */
		if(notify_events && gateway->events_is_enabled()) {
			json_t *info = json_object();
			json_object_set_new(info, "name", json_string(sender->name));
			json_object_set_new(info, "event", json_string("created"));
			json_object_set_new(info, "persistent", json_true());
			if(placeholder_path != NULL)
				json_object_set_new(info, "placeholder", json_string(placeholder_path));
			if(width != -1 && height != -1) {
				json_object_set_new(info, "width", json_integer(width));
				json_object_set_new(info, "height", json_integer(height));

			}
			json_object_set_new(info, "keep_ratio", ratio ? json_true() : json_false());
			gateway->notify_event(&janus_ndi_plugin, NULL, info);
		}
		/* We're done */
		janus_mutex_unlock(&sessions_mutex);
		/* Send response back */
		response = json_object();
		json_object_set_new(response, "ndi", json_string("success"));
		goto prepare_response;
	} else if(!strcasecmp(request_text, "list")) {
		/* Return a list of all existing NDI sessions */
		json_t *list = json_array();
		janus_mutex_lock(&sessions_mutex);
		GHashTableIter iter;
		gpointer value;
		g_hash_table_iter_init(&iter, ndi_names);
		while(g_hash_table_iter_next(&iter, NULL, &value)) {
			janus_ndi_sender *sender = (janus_ndi_sender *)value;
			json_t *s = json_object();
			if(sender->name != NULL)
				json_object_set_new(s, "name", json_string(sender->name));
			json_object_set_new(s, "busy", sender->busy ? json_true() : json_false());
			json_object_set_new(s, "placeholder", sender->placeholder ? json_true() : json_false());
			json_object_set_new(s, "updated", json_integer(sender->last_updated));
			json_array_append_new(list, s);
		}
		/* We're done */
		janus_mutex_unlock(&sessions_mutex);
		/* Send response back */
		response = json_object();
		json_object_set_new(response, "ndi", json_string("success"));
		json_object_set_new(response, "list", list);
		goto prepare_response;
	} else if(!strcasecmp(request_text, "update_img")) {
		JANUS_VALIDATE_JSON_OBJECT(message, updateimg_parameters,
			error_code, error_cause, TRUE,
			JANUS_NDI_ERROR_MISSING_ELEMENT, JANUS_NDI_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto prepare_response;
		/* Destroy a shared NDI instance, but only if it's not busy */
		const char *name = json_string_value(json_object_get(message, "name"));
		if(!strcasecmp(name, test_pattern_name)) {
			/* This is a reserved name */
			JANUS_LOG(LOG_ERR, "This name cannot be used (reserved for test pattern)\n");
			error_code = JANUS_NDI_ERROR_NDI_NAME_IN_USE;
			g_snprintf(error_cause, 512, "This name cannot be used (reserved for test pattern)");
			goto prepare_response;
		}
		/* Make sure this name exists */
		janus_mutex_lock(&sessions_mutex);
		janus_ndi_sender *sender = g_hash_table_lookup(ndi_names, name);
		if(sender == NULL) {
			/* Already in use */
			janus_mutex_unlock(&sessions_mutex);
			JANUS_LOG(LOG_ERR, "No such NDI sender '%s'\n", name);
			error_code = JANUS_NDI_ERROR_NDI_NAME_NOT_FOUND;
			g_snprintf(error_cause, 512, "No such NDI sender '%s'", name);
			goto prepare_response;
		}
		janus_refcount_increase(&sender->ref);
		janus_mutex_unlock(&sessions_mutex);
		/* Get the image to use as a placeholder */
		const char *placeholder_path = json_string_value(json_object_get(message, "placeholder"));
		/* Check if we're forcing a specific resolution for the placeholder */
		int width = -1, height = -1;
		json_t *w = json_object_get(message, "width");
		if(w != NULL) {
			width = json_integer_value(w);
			if(width <= 0 || width > 1920) {
				JANUS_LOG(LOG_WARN, "Invalid target width %d, sticking to actual placeholder resolution\n", width);
				width = -1;
			}
		}
		json_t *h = json_object_get(message, "height");
		if(h != NULL) {
			height = json_integer_value(h);
			if(height <= 0 || height > 1080) {
				JANUS_LOG(LOG_WARN, "Invalid target height %d, sticking to actual placeholder resolution\n", height);
				height = -1;
			}
		}
		json_t *kr = json_object_get(message, "keep_ratio");
		gboolean ratio = kr ? json_is_true(kr) : TRUE;
		/* Generate the placeholder */
		if(janus_ndi_generate_placeholder_image(sender, placeholder_path, width, height, ratio,
				&error_code, error_cause, sizeof(error_cause)) < 0) {
			/* Error code and cause already set by the method */
			goto prepare_response;
		}
		/* We're done */
		janus_refcount_decrease(&sender->ref);
		/* Send response back */
		response = json_object();
		json_object_set_new(response, "ndi", json_string("success"));
		goto prepare_response;
	} else if(!strcasecmp(request_text, "destroy")) {
		JANUS_VALIDATE_JSON_OBJECT(message, destroy_parameters,
			error_code, error_cause, TRUE,
			JANUS_NDI_ERROR_MISSING_ELEMENT, JANUS_NDI_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto prepare_response;
		/* Destroy a shared NDI instance, but only if it's not busy */
		const char *name = json_string_value(json_object_get(message, "name"));
		if(!strcasecmp(name, test_pattern_name)) {
			/* This is a reserved name */
			JANUS_LOG(LOG_ERR, "This name cannot be used (reserved for test pattern)\n");
			error_code = JANUS_NDI_ERROR_NDI_NAME_IN_USE;
			g_snprintf(error_cause, 512, "This name cannot be used (reserved for test pattern)");
			goto prepare_response;
		}
		/* Make sure this name exists */
		janus_mutex_lock(&sessions_mutex);
		janus_ndi_sender *sender = g_hash_table_lookup(ndi_names, name);
		if(sender == NULL) {
			/* Already in use */
			janus_mutex_unlock(&sessions_mutex);
			JANUS_LOG(LOG_ERR, "No such NDI sender '%s'\n", name);
			error_code = JANUS_NDI_ERROR_NDI_NAME_NOT_FOUND;
			g_snprintf(error_cause, 512, "No such NDI sender '%s'", name);
			goto prepare_response;
		}
		if(sender->busy) {
			/* Busy sender */
			janus_mutex_unlock(&sessions_mutex);
			JANUS_LOG(LOG_ERR, "NDI sender is busy\n");
			error_code = JANUS_NDI_ERROR_NDI_ERROR;
			g_snprintf(error_cause, 512, "NDI sender is busy");
			goto prepare_response;
		}
		g_hash_table_remove(ndi_names, name);
		/* We're done */
		janus_mutex_unlock(&sessions_mutex);
		/* Send response back */
		response = json_object();
		json_object_set_new(response, "ndi", json_string("success"));
		goto prepare_response;
	} else if(!strcasecmp(request_text, "start_test_pattern")) {
		JANUS_LOG(LOG_INFO, "Request to start sending the test pattern via NDI\n");
		if(!g_atomic_int_compare_and_exchange(&test_pattern_running, 0, 1)) {
			JANUS_LOG(LOG_VERB, "Test pattern already running\n");
			error_code = JANUS_NDI_ERROR_WRONG_STATE;
			g_snprintf(error_cause, 512, "Test pattern already running");
			goto prepare_response;
		}
		/* Spawn the test pattern thread */
		GError *thread_error = NULL;
		test_pattern_thread = g_thread_try_new("ndi test", &janus_ndi_send_test_pattern, NULL, &thread_error);
		if(thread_error != NULL) {
			JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the test pattern thread...\n",
				thread_error->code, thread_error->message ? thread_error->message : "??");
			g_error_free(thread_error);
			error_code = JANUS_NDI_ERROR_UNKNOWN_ERROR;
			g_snprintf(error_cause, 512, "Couldn't start test pattern thread");
			g_atomic_int_set(&test_pattern_running, 0);
			goto prepare_response;
		}
		/* Send response back */
		response = json_object();
		json_object_set_new(response, "ndi", json_string("success"));
		goto prepare_response;
	} else if(!strcasecmp(request_text, "stop_test_pattern")) {
		JANUS_LOG(LOG_INFO, "Request to stop sending the test pattern via NDI\n");
		if(!g_atomic_int_compare_and_exchange(&test_pattern_running, 1, -1)) {
			JANUS_LOG(LOG_VERB, "Test pattern not running\n");
			error_code = JANUS_NDI_ERROR_WRONG_STATE;
			g_snprintf(error_cause, 512, "Test pattern not running");
			goto prepare_response;
		}
		/* Send response back */
		response = json_object();
		json_object_set_new(response, "ndi", json_string("success"));
		goto prepare_response;
	} else {
		/* Not a request we recognize, don't do anything */
		return NULL;
	}

prepare_response:
		{
			if(error_code == 0 && !response) {
				error_code = JANUS_NDI_ERROR_UNKNOWN_ERROR;
				g_snprintf(error_cause, 512, "Invalid response");
			}
			if(error_code != 0) {
				/* Prepare JSON error event */
				response = json_object();
				json_object_set_new(response, "ndi", json_string("error"));
				json_object_set_new(response, "error_code", json_integer(error_code));
				json_object_set_new(response, "error", json_string(error_cause));
			}
			return response;
		}

}

struct janus_plugin_result *janus_ndi_handle_message(janus_plugin_session *handle, char *transaction, json_t *message, json_t *jsep) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return janus_plugin_result_new(JANUS_PLUGIN_ERROR, g_atomic_int_get(&stopping) ? "Shutting down" : "Plugin not initialized", NULL);

	/* Pre-parse the message */
	int error_code = 0;
	char error_cause[512];
	json_t *root = message;
	json_t *response = NULL;

	janus_mutex_lock(&sessions_mutex);
	janus_ndi_session *session = janus_ndi_lookup_session(handle);
	if(!session) {
		janus_mutex_unlock(&sessions_mutex);
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		error_code = JANUS_NDI_ERROR_UNKNOWN_ERROR;
		g_snprintf(error_cause, 512, "%s", "No session associated with this handle...");
		goto plugin_response;
	}
	/* Increase the reference counter for this session: we'll decrease it after we handle the message */
	janus_refcount_increase(&session->ref);
	janus_mutex_unlock(&sessions_mutex);
	if(g_atomic_int_get(&session->destroyed)) {
		JANUS_LOG(LOG_ERR, "Session has already been destroyed...\n");
		error_code = JANUS_NDI_ERROR_UNKNOWN_ERROR;
		g_snprintf(error_cause, 512, "%s", "Session has already been destroyed...");
		goto plugin_response;
	}

	if(message == NULL) {
		JANUS_LOG(LOG_ERR, "No message??\n");
		error_code = JANUS_NDI_ERROR_NO_MESSAGE;
		g_snprintf(error_cause, 512, "%s", "No message??");
		goto plugin_response;
	}
	if(!json_is_object(root)) {
		JANUS_LOG(LOG_ERR, "JSON error: not an object\n");
		error_code = JANUS_NDI_ERROR_INVALID_JSON;
		g_snprintf(error_cause, 512, "JSON error: not an object");
		goto plugin_response;
	}
	/* Get the request first */
	JANUS_VALIDATE_JSON_OBJECT(root, request_parameters,
		error_code, error_cause, TRUE,
		JANUS_NDI_ERROR_MISSING_ELEMENT, JANUS_NDI_ERROR_INVALID_ELEMENT);
	if(error_code != 0)
		goto plugin_response;
	json_t *request = json_object_get(root, "request");
	/* Some requests ('start_test_pattern' and 'stop_test_pattern') can be handled synchronously */
	const char *request_text = json_string_value(request);
	/* We have a separate method to process synchronous requests, as those may
	 * arrive from the Admin API as well, and so we handle them the same way */
	response = janus_ndi_process_synchronous_request(session, root);
	if(response != NULL) {
		/* We got a response, send it back */
		goto plugin_response;
	} else if(!strcasecmp(request_text, "translate") || !strcasecmp(request_text, "configure") || !strcasecmp(request_text, "hangup")) {
		/* These messages are handled asynchronously */
		janus_ndi_message *msg = g_malloc(sizeof(janus_ndi_message));
		msg->handle = handle;
		msg->transaction = transaction;
		msg->message = root;
		msg->jsep = jsep;

		g_async_queue_push(messages, msg);
		return janus_plugin_result_new(JANUS_PLUGIN_OK_WAIT, NULL, NULL);
	} else {
		JANUS_LOG(LOG_VERB, "Unknown request '%s'\n", request_text);
		error_code = JANUS_NDI_ERROR_INVALID_REQUEST;
		g_snprintf(error_cause, 512, "Unknown request '%s'", request_text);
	}

plugin_response:
		{
			if(error_code == 0 && !response) {
				error_code = JANUS_NDI_ERROR_UNKNOWN_ERROR;
				g_snprintf(error_cause, 512, "Invalid response");
			}
			if(error_code != 0) {
				/* Prepare JSON error event */
				json_t *event = json_object();
				json_object_set_new(event, "ndi", json_string("error"));
				json_object_set_new(event, "error_code", json_integer(error_code));
				json_object_set_new(event, "error", json_string(error_cause));
				response = event;
			}
			if(root != NULL)
				json_decref(root);
			if(jsep != NULL)
				json_decref(jsep);
			g_free(transaction);

			if(session != NULL)
				janus_refcount_decrease(&session->ref);
			return janus_plugin_result_new(JANUS_PLUGIN_OK, NULL, response);
		}

}

json_t *janus_ndi_handle_admin_message(json_t *message) {
	/* Some requests (e.g., 'create' and 'destroy') can be handled via Admin API */
	int error_code = 0;
	char error_cause[512];
	json_t *response = NULL;

	JANUS_VALIDATE_JSON_OBJECT(message, request_parameters,
		error_code, error_cause, TRUE,
		JANUS_NDI_ERROR_MISSING_ELEMENT, JANUS_NDI_ERROR_INVALID_ELEMENT);
	if(error_code != 0)
		goto admin_response;
	json_t *request = json_object_get(message, "request");
	const char *request_text = json_string_value(request);
	if((response = janus_ndi_process_synchronous_request(NULL, message)) != NULL) {
		/* We got a response, send it back */
		goto admin_response;
	} else {
		JANUS_LOG(LOG_VERB, "Unknown request '%s'\n", request_text);
		error_code = JANUS_NDI_ERROR_INVALID_REQUEST;
		g_snprintf(error_cause, 512, "Unknown request '%s'", request_text);
	}

admin_response:
		{
			if(!response) {
				/* Prepare JSON error event */
				response = json_object();
				json_object_set_new(response, "ndi", json_string("error"));
				json_object_set_new(response, "error_code", json_integer(error_code));
				json_object_set_new(response, "error", json_string(error_cause));
			}
			return response;
		}

}

void janus_ndi_setup_media(janus_plugin_session *handle) {
	JANUS_LOG(LOG_INFO, "WebRTC media is now available\n");
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_mutex_lock(&sessions_mutex);
	janus_ndi_session *session = janus_ndi_lookup_session(handle);
	if(!session) {
		janus_mutex_unlock(&sessions_mutex);
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if(g_atomic_int_get(&session->destroyed)) {
		janus_mutex_unlock(&sessions_mutex);
		return;
	}
#if (JANUS_PLUGIN_API_VERSION < 100)
	janus_rtp_switching_context_reset(&session->rtpctx);
#else
	janus_rtp_switching_context_reset(&session->artpctx);
	janus_rtp_switching_context_reset(&session->vrtpctx);
#endif
	g_atomic_int_set(&session->hangingup, 0);
	g_atomic_int_set(&session->hangup, 0);
	janus_mutex_unlock(&sessions_mutex);
}

void janus_ndi_incoming_rtp(janus_plugin_session *handle, janus_plugin_rtp *packet) {
	if(handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	if(gateway) {
		/* Honour the audio/video active flags */
		janus_ndi_session *session = (janus_ndi_session *)handle->plugin_handle;
		if(!session || g_atomic_int_get(&session->destroyed)) {
			JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
			return;
		}
		gboolean video = packet->video;
		char *buf = packet->buffer;
		uint16_t len = packet->length;
		/* Forward to our NDI peer */
		if((video && !session->ctx) || (!video && !session->audiodec)) {
			/* Dropping packet, we don't have a decoder */
			return;
		}
		if(session->ndi_sender == NULL || session->ndi_sender->instance == NULL) {
			/* Dropping packet, we don't have a sender */
			return;
		}
		janus_rtp_header *rtp = (janus_rtp_header *)buf;
		int plen = 0;
		char *payload = janus_rtp_payload(buf, len, &plen);
		if(payload == NULL || plen < 1) {
			/* No payload, drop the packet */
			return;
		}
		if(!video) {
			/* Fix the RTP header, if needed */
#if (JANUS_PLUGIN_API_VERSION < 100)
			janus_rtp_header_update(rtp, &session->rtpctx, FALSE, 0);
#else
			janus_rtp_header_update(rtp, &session->artpctx, FALSE, 0);
#endif
			/* Queue the audio packet (we won't decode now, there might be buffering involved) */
			janus_ndi_buffer_packet *pkt = janus_ndi_buffer_packet_create(buf, len);
			janus_mutex_lock(&session->mutex);
			g_queue_insert_sorted(session->audio_buffered_packets, pkt, (GCompareDataFunc)janus_ndi_buffer_packet_compare, NULL);
			/* If this packet is out-of-order, fix the inserted time */
			if(janus_ndi_rtp_is_outoforder(session, rtp, FALSE)) {
				/* Out of order */
				JANUS_LOG(LOG_WARN, "[%s] Out of order audio packet\n", session->ndi_name);
				GList *item = g_queue_find(session->audio_buffered_packets, pkt);
				janus_ndi_buffer_packet *prev = NULL;
				if(item && item->prev && item->prev->data)
					prev = (janus_ndi_buffer_packet *)item->prev->data;
				else if(item && item->next && item->next->data)
					prev = (janus_ndi_buffer_packet *)item->next->data;
				if(prev != NULL) {
					JANUS_LOG(LOG_HUGE, "[%s]   >> Fixing inserted time: %"SCNi64" --> %"SCNi64"\n",
						session->ndi_name, pkt->inserted, prev->inserted);
					pkt->inserted = prev->inserted;
				}
			}
			janus_mutex_unlock(&session->mutex);
		} else {
			/* Video, check if the timestamp changed: marker bit is not mandatory, and may be lost as well */
			if(session->ctx) {
				/* Fix the RTP header, if needed */
#if (JANUS_PLUGIN_API_VERSION < 100)
				janus_rtp_header_update(rtp, &session->rtpctx, TRUE, 0);
#else
				janus_rtp_header_update(rtp, &session->vrtpctx, TRUE, 0);
#endif
				/* Queue the video packet (we won't decode now, there might be buffering involved) */
				janus_ndi_buffer_packet *pkt = janus_ndi_buffer_packet_create(buf, len);
				janus_mutex_lock(&session->mutex);
				g_queue_insert_sorted(session->video_buffered_packets, pkt, (GCompareDataFunc)janus_ndi_buffer_packet_compare, NULL);
				/* If this packet is out-of-order, fix the inserted time */
				if(janus_ndi_rtp_is_outoforder(session, rtp, TRUE)) {
					/* Out of order */
					JANUS_LOG(LOG_WARN, "[%s] Out of order video packet\n", session->ndi_name);
					GList *item = g_queue_find(session->video_buffered_packets, pkt);
					janus_ndi_buffer_packet *prev = NULL;
					if(item && item->prev && item->prev->data)
						prev = (janus_ndi_buffer_packet *)item->prev->data;
					else if(item && item->next && item->next->data)
						prev = (janus_ndi_buffer_packet *)item->next->data;
					if(prev != NULL) {
						JANUS_LOG(LOG_HUGE, "[%s]   >> Fixing inserted time: %"SCNi64" --> %"SCNi64"\n",
							session->ndi_name, pkt->inserted, prev->inserted);
						pkt->inserted = prev->inserted;
					}
				}
				janus_mutex_unlock(&session->mutex);
			}
		}
	}
}

void janus_ndi_incoming_rtcp(janus_plugin_session *handle, janus_plugin_rtcp *packet) {
	if(handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	if(gateway) {
		janus_ndi_session *session = (janus_ndi_session *)handle->plugin_handle;
		if(!session) {
			JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
			return;
		}
		if(g_atomic_int_get(&session->destroyed))
			return;
		guint32 bitrate = janus_rtcp_get_remb(packet->buffer, packet->length);
		if(bitrate > 0) {
			/* If a REMB arrived, make sure we cap it to our configuration, and send it as a video RTCP */
			gateway->send_remb(handle, session->bitrate ? session->bitrate : 10000000);
			return;
		}
		gateway->relay_rtcp(handle, packet);
	}
}

void janus_ndi_hangup_media(janus_plugin_session *handle) {
	janus_mutex_lock(&sessions_mutex);
	janus_ndi_hangup_media_internal(handle);
	janus_mutex_unlock(&sessions_mutex);
}

static void janus_ndi_hangup_media_internal(janus_plugin_session *handle) {
	JANUS_LOG(LOG_INFO, "No WebRTC media anymore\n");
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_ndi_session *session = janus_ndi_lookup_session(handle);
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if(g_atomic_int_get(&session->destroyed))
		return;
	if(!g_atomic_int_compare_and_exchange(&session->hangingup, 0, 1))
		return;
	g_atomic_int_set(&session->audio, 1);
	g_atomic_int_set(&session->video, 1);
	g_atomic_int_set(&session->paused, 0);
	g_atomic_int_set(&session->hangup, 1);
	g_atomic_int_set(&session->hangingup, 0);
}

/* Thread to handle incoming messages */
static void *janus_ndi_handler(void *data) {
	JANUS_LOG(LOG_VERB, "Joining NDI handler thread\n");
	janus_ndi_message *msg = NULL;
	int error_code = 0;
	char error_cause[512];
	json_t *root = NULL;
	while(g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
		msg = g_async_queue_pop(messages);
		if(msg == &exit_message)
			break;
		if(msg->handle == NULL) {
			janus_ndi_message_free(msg);
			continue;
		}
		janus_mutex_lock(&sessions_mutex);
		janus_ndi_session *session = janus_ndi_lookup_session(msg->handle);
		if(!session) {
			janus_mutex_unlock(&sessions_mutex);
			JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
			janus_ndi_message_free(msg);
			continue;
		}
		if(g_atomic_int_get(&session->destroyed)) {
			janus_mutex_unlock(&sessions_mutex);
			janus_ndi_message_free(msg);
			continue;
		}
		janus_mutex_unlock(&sessions_mutex);
		/* Handle request */
		error_code = 0;
		root = msg->message;
		if(msg->message == NULL) {
			JANUS_LOG(LOG_ERR, "No message??\n");
			error_code = JANUS_NDI_ERROR_NO_MESSAGE;
			g_snprintf(error_cause, 512, "%s", "No message??");
			goto error;
		}
		if(!json_is_object(root)) {
			JANUS_LOG(LOG_ERR, "JSON error: not an object\n");
			error_code = JANUS_NDI_ERROR_INVALID_JSON;
			g_snprintf(error_cause, 512, "JSON error: not an object");
			goto error;
		}
		JANUS_VALIDATE_JSON_OBJECT(root, request_parameters,
			error_code, error_cause, TRUE,
			JANUS_NDI_ERROR_MISSING_ELEMENT, JANUS_NDI_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto error;
		json_t *request = json_object_get(root, "request");
		const char *request_text = json_string_value(request);
		json_t *result = NULL, *localjsep = NULL;

		if(!strcasecmp(request_text, "translate")) {
			JANUS_VALIDATE_JSON_OBJECT(root, translate_parameters,
				error_code, error_cause, TRUE,
				JANUS_NDI_ERROR_MISSING_ELEMENT, JANUS_NDI_ERROR_INVALID_ELEMENT);
			if(error_code != 0)
				goto error;
			/* Validate the ondisconnect, if provided */
			json_t *ondisconnect = json_object_get(root, "ondisconnect");
			if(ondisconnect != NULL) {
				JANUS_VALIDATE_JSON_OBJECT(ondisconnect, ondisconnect_parameters,
					error_code, error_cause, TRUE,
					JANUS_NDI_ERROR_MISSING_ELEMENT, JANUS_NDI_ERROR_INVALID_ELEMENT);
				if(error_code != 0)
					goto error;
			}
			/* Any SDP to handle? If not, something's wrong */
			const char *msg_sdp_type = json_string_value(json_object_get(msg->jsep, "type"));
			const char *msg_sdp = json_string_value(json_object_get(msg->jsep, "sdp"));
			if(!msg_sdp) {
				JANUS_LOG(LOG_ERR, "Missing SDP\n");
				error_code = JANUS_NDI_ERROR_MISSING_SDP;
				g_snprintf(error_cause, 512, "Missing SDP");
				goto error;
			}
			if(!msg_sdp_type || strcasecmp(msg_sdp_type, "offer")) {
				JANUS_LOG(LOG_ERR, "Not an SDP offer\n");
				error_code = JANUS_NDI_ERROR_INVALID_SDP;
				g_snprintf(error_cause, 512, "Missing or invalid SDP type");
				goto error;
			}
			if(strstr(msg_sdp, "m=application")) {
				JANUS_LOG(LOG_ERR, "The NDI plugin does not support DataChannels\n");
				error_code = JANUS_NDI_ERROR_INVALID_SDP;
				g_snprintf(error_cause, 512, "The NDI plugin does not support DataChannels");
				goto error;
			}
			if(json_is_true(json_object_get(msg->jsep, "e2ee"))) {
				/* Media is encrypted, but legacy endpoints will need unencrypted media frames */
				JANUS_LOG(LOG_ERR, "Media encryption unsupported by this plugin\n");
				error_code = JANUS_NDI_ERROR_INVALID_ELEMENT;
				g_snprintf(error_cause, 512, "Media encryption unsupported by this plugin");
				goto error;
			}
			if(session->ndi_name) {
				/* Already connected, or still cleaning up */
				JANUS_LOG(LOG_ERR, "Session already established\n");
				error_code = JANUS_NDI_ERROR_WRONG_STATE;
				g_snprintf(error_cause, 512, "Session already established");
				goto error;
			}
			/* We need an NDI name */
			const char *name = json_string_value(json_object_get(root, "name"));
			if(!strcasecmp(name, test_pattern_name)) {
				/* This is a reserved name */
				JANUS_LOG(LOG_ERR, "This name cannot be used (reserved for test pattern)\n");
				error_code = JANUS_NDI_ERROR_NDI_NAME_IN_USE;
				g_snprintf(error_cause, 512, "This name cannot be used (reserved for test pattern)");
				goto error;
			}
			/* Make sure this name is not in use */
			janus_mutex_lock(&sessions_mutex);
			janus_ndi_sender *sender = g_hash_table_lookup(ndi_names, name);
			if(sender != NULL) {
				/* Already in use: check if it's an external NDI name we can borrow */
				if(!sender->busy) {
					janus_refcount_increase(&sender->ref);
					janus_refcount_increase(&session->ref);
					sender->busy = TRUE;
					session->external_sender = TRUE;
					session->ndi_sender = sender;
				} else {
					janus_mutex_unlock(&sessions_mutex);
					JANUS_LOG(LOG_ERR, "This name is already in use in the plugin\n");
					error_code = JANUS_NDI_ERROR_NDI_NAME_IN_USE;
					g_snprintf(error_cause, 512, "This name is already in use in the plugin");
					goto error;
				}
			} else {
				/* Create a new owned sender */
				session->external_sender = FALSE;
				session->ndi_sender = g_malloc0(sizeof(janus_ndi_sender));
				session->ndi_sender->name = g_strdup(name);
				session->ndi_sender->placeholder = FALSE;
				session->ndi_sender->busy = TRUE;
				janus_refcount_init(&session->ndi_sender->ref, janus_ndi_sender_free);
				janus_mutex_init(&session->ndi_sender->mutex);
				g_hash_table_insert(ndi_names, g_strdup(name), session->ndi_sender);
			}
			g_free(session->ndi_name);
			session->ndi_name = g_strdup(name);
			janus_mutex_unlock(&sessions_mutex);
			/* Also check if we need to send some metadata */
			json_t *m = json_object_get(root, "metadata");
			if(m != NULL) {
				const char *metadata = json_string_value(m);
				g_free(session->ndi_metadata);
				session->ndi_metadata = metadata ? g_strdup(metadata) : NULL;
			}
			/* Check if we're capping to a specific bitrate */
			json_t *b = json_object_get(root, "bitrate");
			session->bitrate = b ? json_integer_value(b) : 0;
			/* Check if we're forcing a specific resolution */
			int width = -1, height = -1;
			json_t *w = json_object_get(root, "width");
			if(w != NULL) {
				width = json_integer_value(w);
				if(width <= 0 || width > 1920) {
					JANUS_LOG(LOG_WARN, "Invalid target width %d, sticking to actual video resolution\n", width);
					width = -1;
				}
			}
			json_t *h = json_object_get(root, "height");
			if(h != NULL) {
				height = json_integer_value(h);
				if(height <= 0 || height > 1080) {
					JANUS_LOG(LOG_WARN, "Invalid target height %d, sticking to actual video resolution\n", height);
					height = -1;
				}
			}
			json_t *fps = json_object_get(root, "fps");
			session->fps = fps ? json_integer_value(h) : 0;
			/* Parse the SDP we got one */
			char sdperror[100];
			janus_sdp *offer = janus_sdp_parse(msg_sdp, sdperror, sizeof(sdperror));
			if(!offer) {
				janus_mutex_lock(&sessions_mutex);
				if(!session->ndi_sender->placeholder) {
					g_hash_table_remove(ndi_names, name);
				} else {
					session->external_sender = FALSE;
					sender->busy = FALSE;
					janus_refcount_decrease(&session->ndi_sender->ref);
					janus_refcount_decrease(&session->ref);
				}
				session->ndi_sender = NULL;
				janus_mutex_unlock(&sessions_mutex);
				JANUS_LOG(LOG_ERR, "Error parsing SDP: %s\n", sdperror);
				error_code = JANUS_NDI_ERROR_INVALID_SDP;
				g_snprintf(error_cause, 512, "Error parsing SDP: %s", sdperror);
				goto error;
			}
			json_t *videocodec = json_object_get(root, "videocodec");
			/* Generate an answer */
#if (JANUS_PLUGIN_API_VERSION < 100)
			janus_sdp *answer = janus_sdp_generate_answer(offer,
				JANUS_SDP_OA_AUDIO, TRUE,
				JANUS_SDP_OA_AUDIO_CODEC, "opus",
				JANUS_SDP_OA_AUDIO_DIRECTION, JANUS_SDP_RECVONLY,
				JANUS_SDP_OA_AUDIO_FMTP, "stereo=1",
				JANUS_SDP_OA_VIDEO, TRUE,
				JANUS_SDP_OA_VIDEO_CODEC, json_string_value(videocodec),
				JANUS_SDP_OA_VIDEO_DIRECTION, JANUS_SDP_RECVONLY,
				JANUS_SDP_OA_DATA, FALSE,
				JANUS_SDP_OA_ACCEPT_EXTMAP, JANUS_RTP_EXTMAP_MID,
				JANUS_SDP_OA_ACCEPT_EXTMAP, JANUS_RTP_EXTMAP_TRANSPORT_WIDE_CC,
				JANUS_SDP_OA_DONE);
#else
			janus_sdp *answer = janus_sdp_generate_answer(offer);
			gboolean audio_accepted = FALSE, video_accepted = FALSE;
			GList *temp = offer->m_lines;
			while(temp) {
				janus_sdp_mline *m = (janus_sdp_mline *)temp->data;
				if(m->type == JANUS_SDP_AUDIO && !audio_accepted) {
					audio_accepted = TRUE;
					janus_sdp_generate_answer_mline(offer, answer, m,
						JANUS_SDP_OA_MLINE, JANUS_SDP_AUDIO,
							JANUS_SDP_OA_CODEC, "opus",
							JANUS_SDP_OA_DIRECTION, JANUS_SDP_RECVONLY,
							JANUS_SDP_OA_FMTP, "stereo=1",
							JANUS_SDP_OA_ACCEPT_EXTMAP, JANUS_RTP_EXTMAP_MID,
							JANUS_SDP_OA_ACCEPT_EXTMAP, JANUS_RTP_EXTMAP_TRANSPORT_WIDE_CC,
						JANUS_SDP_OA_DONE);
				} else if(m->type == JANUS_SDP_VIDEO && !video_accepted) {
					video_accepted = TRUE;
					janus_sdp_generate_answer_mline(offer, answer, m,
						JANUS_SDP_OA_MLINE, JANUS_SDP_VIDEO,
							JANUS_SDP_OA_CODEC, json_string_value(videocodec),
							JANUS_SDP_OA_DIRECTION, JANUS_SDP_RECVONLY,
							JANUS_SDP_OA_ACCEPT_EXTMAP, JANUS_RTP_EXTMAP_MID,
							JANUS_SDP_OA_ACCEPT_EXTMAP, JANUS_RTP_EXTMAP_TRANSPORT_WIDE_CC,
						JANUS_SDP_OA_DONE);
				}
				temp = temp->next;
			}
#endif
			janus_sdp_destroy(offer);
			/* Check which decoders we need */
			const char *acodec = NULL, *vcodec = NULL;
#if (JANUS_PLUGIN_API_VERSION < 100)
			janus_sdp_find_first_codecs(answer, &acodec, &vcodec);
#else
			janus_sdp_find_first_codec(answer, JANUS_SDP_AUDIO, -1, &acodec);
			janus_sdp_find_first_codec(answer, JANUS_SDP_VIDEO, -1, &vcodec);
#endif
			if(acodec && janus_audiocodec_from_name(acodec) == JANUS_AUDIOCODEC_OPUS) {
				/* Create the Opus decoder */
				int opus_error = 0;
				session->audiodec = opus_decoder_create(48000, 2, &opus_error);
				if(opus_error != OPUS_OK) {
					/* FIXME We ignore this error for now */
					JANUS_LOG(LOG_ERR, "Error creating Opus decoder: %d\n", opus_error);
				}
			}
			if(vcodec) {
				session->vcodec = janus_videocodec_from_name(vcodec);
				if(session->vcodec != JANUS_VIDEOCODEC_NONE) {
					/* Create the video decoder */
					const AVCodec *codec = avcodec_find_decoder_by_name(session->vcodec == JANUS_VIDEOCODEC_AV1 ? "libaom-av1" : vcodec);
					if(codec == NULL) {
						/* FIXME We ignore this error for now */
						JANUS_LOG(LOG_ERR, "%s decoder not available\n", vcodec);
						opus_decoder_destroy(session->audiodec);
						session->audiodec = NULL;
						session->vcodec = JANUS_VIDEOCODEC_NONE;
					} else {
						session->ctx = avcodec_alloc_context3(codec);
						if(session->ctx == NULL) {
							/* FIXME We ignore this error for now */
							JANUS_LOG(LOG_ERR, "Error creating decoder\n");
							opus_decoder_destroy(session->audiodec);
							session->audiodec = NULL;
							session->vcodec = JANUS_VIDEOCODEC_NONE;
						} else {
							session->ctx->coded_width = 320;	/* Not relevant */
							session->ctx->coded_height = 240;	/* Not relevant */
							session->width = 0;
							session->height = 0;
							session->target_width = 0;
							session->target_height = 0;
							if(width != -1 && height != -1) {
								session->target_width = width;
								session->target_height = height;
							}
							if(avcodec_open2(session->ctx, codec, NULL) < 0) {
								/* FIXME We ignore this error for now */
								JANUS_LOG(LOG_ERR, "Error opening video decoder...\n");
								opus_decoder_destroy(session->audiodec);
								session->audiodec = NULL;
								avcodec_free_context(&session->ctx);
								av_free(session->ctx);
								session->ctx = NULL;
								session->vcodec = JANUS_VIDEOCODEC_NONE;
							}
						}
					}
				}
			}
			/* Create an NDI sender */
			if(!session->external_sender && (session->audiodec || session->ctx)) {
				NDIlib_send_create_t NDI_send_create_desc = {0};
				NDI_send_create_desc.p_ndi_name = session->ndi_sender->name;
				session->ndi_sender->instance = NDIlib_send_create(&NDI_send_create_desc);
				if(session->ndi_sender->instance == NULL) {
					/* FIXME We ignore this error for now */
					JANUS_LOG(LOG_ERR, "Error creating NDI source for '%s'\n", name);
					g_free(session->ndi_name);
					session->ndi_name = NULL;
					opus_decoder_destroy(session->audiodec);
					session->audiodec = NULL;
					avcodec_free_context(&session->ctx);
					av_free(session->ctx);
					session->ctx = NULL;
				}
				/* Also notify event handlers */
				if(notify_events && gateway->events_is_enabled()) {
					json_t *info = json_object();
					json_object_set_new(info, "name", json_string(session->ndi_name));
					json_object_set_new(info, "event", json_string("created"));
					gateway->notify_event(&janus_ndi_plugin, session->handle, info);
				}
			}
			/* Add metadata, if required */
			janus_mutex_lock(&session->ndi_sender->mutex);
			NDIlib_send_clear_connection_metadata(session->ndi_sender->instance);
			if(session->ndi_metadata) {
				NDIlib_metadata_frame_t NDI_product_type;
				NDI_product_type.p_data = session->ndi_metadata;
				NDIlib_send_add_connection_metadata(session->ndi_sender->instance, &NDI_product_type);
			}
			janus_mutex_unlock(&session->ndi_sender->mutex);
			/* Create a queue for buffered packets */
			session->audio_buffered_packets = g_queue_new();
			session->video_buffered_packets = g_queue_new();
			/* Take note of which image to use on disconnect, if provided */
			if(ondisconnect) {
				const char *d_path = json_string_value(json_object_get(ondisconnect, "image"));
				const char *d_color = json_string_value(json_object_get(ondisconnect, "color"));
				if(d_color && *d_color != '#') {
					JANUS_LOG(LOG_WARN, "Invalid color '%s', falling back to '#000000'\n", d_color);
					d_color = "#000000";
				}
				session->disconnected = g_strdup(d_path);
				if(d_color)
					session->disconnected_color = g_strdup(d_color + 1);
			}
			/* By default we relay both audio and video */
			g_atomic_int_set(&session->audio, 1);
			g_atomic_int_set(&session->video, 1);
			json_t *a = json_object_get(root, "audio");
			if(a != NULL)
				g_atomic_int_set(&session->audio, json_is_true(a));
			json_t *v = json_object_get(root, "video");
			if(v != NULL)
				g_atomic_int_set(&session->video, json_is_true(v));
			/* Check if we should be strict when decoding video */
			json_t *strict = json_object_get(root, "strict");
			session->strict_decoder = strict ? json_is_true(strict) : FALSE;

			/* Spawn a thread */
			g_atomic_int_set(&session->hangup, 0);
			janus_refcount_increase(&session->ref);
			const char *warning = NULL;
			GError *thread_error = NULL;
			char tname[16];
			g_snprintf(tname, sizeof(tname), "session %s", session->ndi_name);
			session->thread = g_thread_try_new(tname, &janus_ndi_processing_thread, session, &thread_error);
			if(thread_error != NULL) {
				/* FIXME We ignore this error for now */
				warning = "Error launching thread";
				JANUS_LOG(LOG_ERR, "[%s] Got error %d (%s) trying to launch the thread...\n",
					session->ndi_name, thread_error->code, thread_error->message ? thread_error->message : "??");
				janus_refcount_decrease(&session->ref);
				g_error_free(thread_error);
			}
			/* Take note of the SDP (may be useful for UPDATEs or re-INVITEs) */
			janus_sdp_destroy(session->sdp);
			session->sdp = answer;
			char *sdp = janus_sdp_write(answer);
			JANUS_LOG(LOG_VERB, "Prepared SDP answer for %p\n%s", name, sdp);
			g_atomic_int_set(&session->hangingup, 0);
			/* Send SDP to the browser */
			result = json_object();
			json_object_set_new(result, "event", json_string("translating"));
			if(warning != NULL)
				json_object_set_new(result, "warning", json_string(warning));
			localjsep = json_pack("{ssss}", "type", "answer", "sdp", sdp);
			g_free(sdp);
		} else if(!strcasecmp(request_text, "configure")) {
			JANUS_VALIDATE_JSON_OBJECT(root, configure_parameters,
				error_code, error_cause, TRUE,
				JANUS_NDI_ERROR_MISSING_ELEMENT, JANUS_NDI_ERROR_INVALID_ELEMENT);
			if(error_code != 0)
				goto error;
			if(json_is_true(json_object_get(root, "keyframe"))) {
				/* Send a PLI */
				JANUS_LOG(LOG_VERB, "[%s] Sending PLI\n", session->ndi_name);
				gateway->send_pli(session->handle);
			}
			json_t *b = json_object_get(root, "bitrate");
			if(b != NULL) {
				session->bitrate = json_integer_value(b);
				JANUS_LOG(LOG_VERB, "[%s] Setting video bitrate: %"SCNu32"\n", session->ndi_name, session->bitrate);
				gateway->send_remb(session->handle, session->bitrate ? session->bitrate : 10000000);
			}
			result = json_object();
			json_t *p = json_object_get(root, "paused");
			if(p != NULL)
				g_atomic_int_set(&session->paused, json_is_true(p));
			json_t *a = json_object_get(root, "audio");
			if(a != NULL)
				g_atomic_int_set(&session->audio, json_is_true(a));
			json_t *v = json_object_get(root, "video");
			if(v != NULL)
				g_atomic_int_set(&session->video, json_is_true(v));
			json_object_set_new(result, "event", json_string("configured"));
		} else if(!strcasecmp(request_text, "hangup")) {
			/* Get rid of an ongoing session */
			gateway->close_pc(session->handle);
			result = json_object();
			json_object_set_new(result, "event", json_string("hangingup"));
		} else {
			JANUS_LOG(LOG_ERR, "Unknown request (%s)\n", request_text);
			error_code = JANUS_NDI_ERROR_INVALID_REQUEST;
			g_snprintf(error_cause, 512, "Unknown request (%s)", request_text);
			goto error;
		}

		/* Prepare JSON event */
		json_t *event = json_object();
		json_object_set_new(event, "ndi", json_string("event"));
		if(result != NULL)
			json_object_set_new(event, "result", result);
		int ret = gateway->push_event(msg->handle, &janus_ndi_plugin, msg->transaction, event, localjsep);
		JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (%s)\n", ret, janus_get_api_error(ret));
		json_decref(event);
		if(localjsep)
			json_decref(localjsep);
		janus_ndi_message_free(msg);
		continue;

error:
		{
			/* Prepare JSON error event */
			json_t *event = json_object();
			json_object_set_new(event, "ndi", json_string("event"));
			json_object_set_new(event, "error_code", json_integer(error_code));
			json_object_set_new(event, "error", json_string(error_cause));
			int ret = gateway->push_event(msg->handle, &janus_ndi_plugin, msg->transaction, event, NULL);
			JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (%s)\n", ret, janus_get_api_error(ret));
			json_decref(event);
			janus_ndi_message_free(msg);
		}
	}
	JANUS_LOG(LOG_VERB, "Leaving NDI handler thread\n");
	return NULL;
}

/* Helpers to decode Exp-Golomb */
static uint32_t janus_ndi_h264_eg_getbit(uint8_t *base, uint32_t offset) {
	return ((*(base + (offset >> 0x3))) >> (0x7 - (offset & 0x7))) & 0x1;
}
static uint32_t janus_ndi_h264_eg_decode(uint8_t *base, uint32_t *offset) {
	uint32_t zeros = 0;
	while(janus_ndi_h264_eg_getbit(base, (*offset)++) == 0)
		zeros++;
	uint32_t res = 1 << zeros;
	int32_t i = 0;
	for(i=zeros-1; i>=0; i--) {
		res |= janus_ndi_h264_eg_getbit(base, (*offset)++) << i;
	}
	return res-1;
}
/* Helper to parse a SPS (only to get the video resolution) */
static void janus_ndi_h264_parse_sps(char *buffer, int *width, int *height) {
	/* Let's check if it's the right profile, first */
	int index = 1;
	int profile_idc = *(buffer+index);
	if(profile_idc != 66) {
		JANUS_LOG(LOG_WARN, "Profile is not baseline (%d != 66)\n", profile_idc);
	}
	/* Then let's skip 2 bytes and evaluate/skip the rest */
	index += 3;
	uint32_t offset = 0;
	uint8_t *base = (uint8_t *)(buffer+index);
	/* Skip seq_parameter_set_id and log2_max_frame_num_minus4 */
	janus_ndi_h264_eg_decode(base, &offset);
	janus_ndi_h264_eg_decode(base, &offset);
	/* Evaluate pic_order_cnt_type */
	int pic_order_cnt_type = janus_ndi_h264_eg_decode(base, &offset);
	if(pic_order_cnt_type == 0) {
		/* Skip log2_max_pic_order_cnt_lsb_minus4 */
		janus_ndi_h264_eg_decode(base, &offset);
	} else if(pic_order_cnt_type == 1) {
		/* Skip delta_pic_order_always_zero_flag, offset_for_non_ref_pic,
		 * offset_for_top_to_bottom_field and num_ref_frames_in_pic_order_cnt_cycle */
		janus_ndi_h264_eg_getbit(base, offset++);
		janus_ndi_h264_eg_decode(base, &offset);
		janus_ndi_h264_eg_decode(base, &offset);
		int num_ref_frames_in_pic_order_cnt_cycle = janus_ndi_h264_eg_decode(base, &offset);
		int i = 0;
		for(i=0; i<num_ref_frames_in_pic_order_cnt_cycle; i++) {
			janus_ndi_h264_eg_decode(base, &offset);
		}
	}
	/* Skip max_num_ref_frames and gaps_in_frame_num_value_allowed_flag */
	janus_ndi_h264_eg_decode(base, &offset);
	janus_ndi_h264_eg_getbit(base, offset++);
	/* We need the following three values */
	int pic_width_in_mbs_minus1 = janus_ndi_h264_eg_decode(base, &offset);
	int pic_height_in_map_units_minus1 = janus_ndi_h264_eg_decode(base, &offset);
	int frame_mbs_only_flag = janus_ndi_h264_eg_getbit(base, offset++);
	if(!frame_mbs_only_flag) {
		/* Skip mb_adaptive_frame_field_flag */
		janus_ndi_h264_eg_getbit(base, offset++);
	}
	/* Skip direct_8x8_inference_flag */
	janus_ndi_h264_eg_getbit(base, offset++);
	/* We need the following value to evaluate offsets, if any */
	int frame_cropping_flag = janus_ndi_h264_eg_getbit(base, offset++);
	int frame_crop_left_offset = 0, frame_crop_right_offset = 0,
		frame_crop_top_offset = 0, frame_crop_bottom_offset = 0;
	if(frame_cropping_flag) {
		frame_crop_left_offset = janus_ndi_h264_eg_decode(base, &offset);
		frame_crop_right_offset = janus_ndi_h264_eg_decode(base, &offset);
		frame_crop_top_offset = janus_ndi_h264_eg_decode(base, &offset);
		frame_crop_bottom_offset = janus_ndi_h264_eg_decode(base, &offset);
	}
	/* Skip vui_parameters_present_flag */
	janus_ndi_h264_eg_getbit(base, offset++);

	/* We skipped what we didn't care about and got what we wanted, compute width/height */
	if(width)
		*width = ((pic_width_in_mbs_minus1 +1)*16) - frame_crop_bottom_offset*2 - frame_crop_top_offset*2;
	if(height)
		*height = ((2 - frame_mbs_only_flag)* (pic_height_in_map_units_minus1 +1) * 16) - (frame_crop_right_offset * 2) - (frame_crop_left_offset * 2);
}

/* Helper to decode a leb128 integer  */
static uint32_t janus_ndi_av1_lev128_decode(uint8_t *base, uint16_t maxlen, size_t *read) {
	uint32_t val = 0;
	uint8_t *cur = base;
	while((cur-base) < maxlen) {
		/* We only read the 7 least significant bits of each byte */
		val |= ((uint32_t)(*cur & 0x7f)) << ((cur-base)*7);
		if((*cur & 0x80) == 0) {
			/* Most significant bit is 0, we're done */
			*read = (cur-base)+1;
			return val;
		}
		cur++;
	}
	/* If we got here, we read all bytes, but no one with 0 as MSB? */
	return 0;
}
/* Helper to encode a leb128 integer  */
static void janus_ndi_av1_lev128_encode(uint32_t value, uint8_t *base, size_t *written) {
	uint8_t *cur = base;
	while(value >= 0x80) {
		/* All these bytes need MSB=1 */
		*cur = (0x80 | (value & 0x7F));
		cur++;
		value >>= 7;
	}
	/* Last byte will have MSB=0 */
	*cur = value;
	*written = (cur-base)+1;
}
/* Helpers to read a bit, or group of bits, in a Sequence Header */
static uint32_t janus_ndi_av1_getbit(uint8_t *base, uint32_t offset) {
	return ((*(base + (offset >> 0x3))) >> (0x7 - (offset & 0x7))) & 0x1;
}
static uint32_t janus_ndi_av1_getbits(uint8_t *base, uint8_t num, uint32_t *offset) {
	uint32_t res = 0;
	int32_t i = 0;
	for(i=num-1; i>=0; i--) {
		res |= janus_ndi_av1_getbit(base, (*offset)++) << i;
	}
	return res;
}
/* Helper to parse a Sequence Header (only to get the video resolution) */
static void janus_ndi_av1_parse_sh(char *buffer, uint16_t *width, uint16_t *height) {
	/* Evaluate/skip everything until we get to the resolution */
	uint32_t offset = 0, value = 0, i = 0;
	uint8_t *base = (uint8_t *)(buffer);
	/* Skip seq_profile (3 bits) */
	janus_ndi_av1_getbits(base, 3, &offset);
	/* Skip still_picture (1 bit) */
	janus_ndi_av1_getbit(base, offset++);
	/* Skip reduced_still_picture_header (1 bit) */
	value = janus_ndi_av1_getbit(base, offset++);
	if(value) {
		/* Skip seq_level_idx (5 bits) */
		janus_ndi_av1_getbits(base, 5, &offset);
	} else {
		gboolean decoder_model_info = FALSE, initial_display_delay = FALSE;
		uint32_t bdlm1 = 0;
		/* Skip timing_info_present_flag (1 bit) */
		value = janus_ndi_av1_getbit(base, offset++);
		if(value) {
			/* Skip num_units_in_display_tick (32 bits) */
			janus_ndi_av1_getbits(base, 32, &offset);
			/* Skip time_scale (32 bits) */
			janus_ndi_av1_getbits(base, 32, &offset);
			/* Skip equal_picture_interval (1 bit)*/
			value = janus_ndi_av1_getbit(base, offset++);
			if(value) {
				/* TODO Skip num_ticks_per_picture_minus_1 (uvlc) */
			}
			/* Skip decoder_model_info_present_flag (1 bit) */
			value = janus_ndi_av1_getbit(base, offset++);
			if(value) {
				decoder_model_info = TRUE;
				/* Skip buffer_delay_length_minus_1 (5 bits) */
				bdlm1 = janus_ndi_av1_getbits(base, 5, &offset);
				/* Skip num_units_in_decoding_tick (32 bits) */
				janus_ndi_av1_getbits(base, 32, &offset);
				/* Skip buffer_removal_time_length_minus_1 (5 bits) */
				janus_ndi_av1_getbits(base, 5, &offset);
				/* Skip frame_presentation_time_length_minus_1 (5 bits) */
				janus_ndi_av1_getbits(base, 5, &offset);
			}
		}
		/* Skip initial_display_delay_present_flag (1 bit) */
		value = janus_ndi_av1_getbit(base, offset++);
		if(value)
			initial_display_delay = TRUE;
		/* Skip operating_points_cnt_minus_1 (5 bits) */
		uint32_t opcm1 = janus_ndi_av1_getbits(base, 5, &offset)+1;
		for(i=0; i<opcm1; i++) {
			/* Skip operating_point_idc[i] (12 bits) */
			janus_ndi_av1_getbits(base, 12, &offset);
			/* Skip seq_level_idx[i] (5 bits) */
			value = janus_ndi_av1_getbits(base, 5, &offset);
			if(value > 7) {
				/* Skip seq_tier[i] (1 bit) */
				janus_ndi_av1_getbit(base, offset++);
			}
			if(decoder_model_info) {
				/* Skip decoder_model_present_for_this_op[i] (1 bit) */
				value = janus_ndi_av1_getbit(base, offset++);
				if(value) {
					/* Skip operating_parameters_info(i) */
					janus_ndi_av1_getbits(base, (2*bdlm1)+1, &offset);
				}
			}
			if(initial_display_delay) {
				/* Skip initial_display_delay_present_for_this_op[i] (1 bit) */
				value = janus_ndi_av1_getbit(base, offset++);
				if(value) {
					/* Skip initial_display_delay_minus_1[i] (4 bits) */
					janus_ndi_av1_getbits(base, 4, &offset);
				}
			}
		}
	}
	/* Read frame_width_bits_minus_1 (4 bits) */
	uint32_t fwbm1 = janus_ndi_av1_getbits(base, 4, &offset);
	/* Read frame_height_bits_minus_1 (4 bits) */
	uint32_t fhbm1 = janus_ndi_av1_getbits(base, 4, &offset);
	/* Read max_frame_width_minus_1 (n bits) */
	*width = janus_ndi_av1_getbits(base, fwbm1+1, &offset)+1;
	/* Read max_frame_height_minus_1 (n bits) */
	*height = janus_ndi_av1_getbits(base, fhbm1+1, &offset)+1;
}

/* Audio/video processing thread */
static void *janus_ndi_processing_thread(void *data) {
	janus_ndi_session *session = (janus_ndi_session *)data;
	if(!session) {
		JANUS_LOG(LOG_ERR, "Invalid session, leaving thread...\n");
		g_thread_unref(g_thread_self());
		return NULL;
	}
	JANUS_LOG(LOG_INFO, "[%s] Starting session thread\n", session->ndi_name);

	/* Stuff */
	char *packet = NULL, *payload = NULL;
	int bytes = 0, plen = 0;
	opus_int16 opus_samples[960*4];

	/* Video decoding stuff */
	int canvas_size = 256000;	/* FIXME */
	uint8_t *received_frame = g_malloc0(canvas_size);
	uint8_t *obu_data = (session->vcodec == JANUS_VIDEOCODEC_AV1 ? g_malloc0(canvas_size) : NULL);
	int frame_len = 0, data_len = 0;
	guint32 prev_ts = 0, last_ts = 0;
	gboolean prevts_set = FALSE, ts_changed = FALSE, got_video = FALSE, got_keyframe = FALSE, key_frame = FALSE;
	uint16_t max_seq_nr = 0;
	uint8_t gaps = 0;
	gboolean waiting_kf = FALSE;
	int width = 0, height = 0;
	AVFrame *frame = NULL, *decoded_frame = av_frame_alloc(), *scaled_frame = NULL, *canvas = NULL;
	struct SwsContext *sws = NULL, *sws_canvas = NULL;
	gint64 last_pli = 0;
	gboolean need_pli = FALSE;

	/* Tally monitoring and state */
	gboolean tally_preview, tally_program;
	gint64 tally_last_poll = 0;

	/* Timers*/
	gboolean done_something = TRUE;
	gint64 now = 0, destroyed = 0;

	/* Also notify event handlers */
	if(notify_events && gateway->events_is_enabled()) {
		json_t *info = json_object();
		json_object_set_new(info, "name", json_string(session->ndi_name));
		json_object_set_new(info, "event", json_string("starting"));
		gateway->notify_event(&janus_ndi_plugin, session->handle, info);
	}

	while(session) {
		/* If the user has been removed, we need to wrap up */
		now = g_get_monotonic_time();
		if((g_atomic_int_get(&session->destroyed) || g_atomic_int_get(&session->hangup)) && destroyed == 0) {
			JANUS_LOG(LOG_INFO, "[%s] Marking session thread as destroyed\n", session->ndi_name);
			destroyed = now;
		}
		if(destroyed && (now - destroyed) >= buffer_size)
			break;
		if(!done_something) {
			/* No packet in the previous iteration, sleep a bit */
			g_usleep(5000);
		}
		done_something = FALSE;

		/* Do we have a PLI to send? */
		if(need_pli && (now-last_pli >= G_USEC_PER_SEC)) {
			JANUS_LOG(LOG_INFO, "[%s] Sending PLI\n", session->ndi_name);
			last_pli = now;
			need_pli = FALSE;
			gateway->send_pli(session->handle);
		}

		/* Check if it's time to poll the tally (we query once a second) */
		if(tally_last_poll == 0)
			tally_last_poll = now;
		if(now-tally_last_poll >= G_USEC_PER_SEC) {
			tally_last_poll = now;
			NDIlib_tally_t tally_info = { 0 };
			NDIlib_send_get_tally(session->ndi_sender->instance, &tally_info, 0);
			if(tally_preview != tally_info.on_preview || tally_program != tally_info.on_program) {
				/* Something changed, notify */
				tally_preview = tally_info.on_preview;
				tally_program = tally_info.on_program;
				JANUS_LOG(LOG_VERB, "[%s] Tally: preview=%d, program=%d\n", session->ndi_name, tally_preview, tally_program);
				/* Prepare JSON event */
				json_t *event = json_object();
				json_object_set_new(event, "ndi", json_string("event"));
				json_t *result = json_object();
				json_object_set_new(result, "event", json_string("tally"));
				json_object_set_new(result, "preview", tally_preview ? json_true() : json_false());
				json_object_set_new(result, "program", tally_program ? json_true() : json_false());
				json_object_set_new(event, "result", result);
				gateway->push_event(session->handle, &janus_ndi_plugin, NULL, event, NULL);
				json_decref(event);
				/* Also notify event handlers */
				if(notify_events && gateway->events_is_enabled()) {
					json_t *info = json_object();
					json_object_set_new(info, "event", json_string("tally"));
					json_object_set_new(info, "name", json_string(session->ndi_name));
					json_object_set_new(info, "preview", tally_preview ? json_true() : json_false());
					json_object_set_new(info, "program", tally_program ? json_true() : json_false());
					gateway->notify_event(&janus_ndi_plugin, session->handle, info);
				}
			}
		}

		/* Let's start with audio */
		janus_mutex_lock(&session->mutex);
		janus_ndi_buffer_packet *pkt = session->audio_buffered_packets ? g_queue_peek_head(session->audio_buffered_packets) : NULL;
		janus_mutex_unlock(&session->mutex);
		while(pkt != NULL && ((now - pkt->inserted) >= buffer_size)) {
			JANUS_LOG(LOG_HUGE, "[%s] Decoding Opus packet (audio)\n", session->ndi_name);
			packet = NULL;
			bytes = 0;
			done_something = TRUE;
			janus_mutex_lock(&session->mutex);
			pkt = g_queue_pop_head(session->audio_buffered_packets);
			janus_mutex_unlock(&session->mutex);
			/* We need this packet now, decode it */
			packet = pkt->buffer;
			bytes = pkt->len;
			payload = janus_rtp_payload(packet, bytes, &plen);
			/* Decode the audio packet */
			int res = opus_decode(session->audiodec, (const unsigned char *)payload, plen,
				opus_samples, 960*4, 0);
			if(res < 0) {
				JANUS_LOG(LOG_ERR, "[%s] Ops! got an error decoding the Opus frame (%d bytes): %d (%s)\n",
					session->ndi_name, plen, res, opus_strerror(res));
			} else if(g_atomic_int_get(&session->audio) && !g_atomic_int_get(&session->paused)) {
				/* Send via NDI as interleaved audio */
				NDIlib_audio_frame_interleaved_16s_t NDI_audio_frame = { 0 };
				NDI_audio_frame.sample_rate = 48000;
				NDI_audio_frame.no_channels = 2;
				NDI_audio_frame.no_samples = 960;
				NDI_audio_frame.p_data = (short *)opus_samples;
				NDI_audio_frame.timecode = NDIlib_send_timecode_synthesize;
				janus_mutex_lock(&session->ndi_sender->mutex);
				NDIlib_util_send_send_audio_interleaved_16s(session->ndi_sender->instance, &NDI_audio_frame);
				janus_mutex_unlock(&session->ndi_sender->mutex);
			}
			/* Get rid of the buffered packet */
			janus_ndi_buffer_packet_destroy(pkt);
			/* Peek the next packet */
			janus_mutex_lock(&session->mutex);
			pkt = g_queue_peek_head(session->audio_buffered_packets);
			janus_mutex_unlock(&session->mutex);
		}
		/* Now move to video */
		janus_mutex_lock(&session->mutex);
		pkt = session->video_buffered_packets ? g_queue_peek_head(session->video_buffered_packets) : NULL;
		janus_mutex_unlock(&session->mutex);
		if(pkt != NULL && ((now - pkt->inserted) >= buffer_size)) {
			/* Time to decode this packet(s), get all the packets with the same timestamp */
			last_ts = pkt->timestamp;
			if(prevts_set) {
				/* The previous round didn't give us a complete frame, keep looking for the same timestamp */
				prevts_set = FALSE;
				last_ts = prev_ts;
			} else {
				gaps = 0;
			}
			while(pkt != NULL) {
				packet = NULL;
				bytes = 0;
				janus_mutex_lock(&session->mutex);
				pkt = g_queue_peek_head(session->video_buffered_packets);
				janus_mutex_unlock(&session->mutex);
				if(pkt == NULL || ((now - pkt->inserted) < buffer_size))
					break;
				/* Decode the packet */
				packet = pkt->buffer;
				bytes = pkt->len;
				janus_rtp_header *rtp = (janus_rtp_header *)packet;
				if(ntohl(rtp->timestamp) == last_ts) {
					/* Timestamp we're interested in, pop the packet */
					done_something = TRUE;
					janus_mutex_lock(&session->mutex);
					(void)g_queue_pop_head(session->video_buffered_packets);
					janus_mutex_unlock(&session->mutex);
					JANUS_LOG(LOG_HUGE, "[%s] Processing video RTP packet: ts=%"SCNu32", seq=%"SCNu16", ins=%"SCNu64"\n",
						session->ndi_name, pkt->timestamp, pkt->seq_number, pkt->inserted);
					if(!prevts_set) {
						/* Let's keep track of this timestamp */
						prevts_set = TRUE;
						prev_ts = last_ts;
					}
					/* Also check if there's gaps in the sequence number */
					if(session->strict_decoder && (int16_t)(pkt->seq_number - max_seq_nr) > 1) {
						/* FIXME Should we drop this packet? */
						gaps++;
						JANUS_LOG(LOG_WARN, "[%s] Detected missing packet (%"SCNu16", expecting %"SCNu16")\n",
							session->ndi_name, pkt->seq_number, (max_seq_nr+1));
					}
					max_seq_nr = pkt->seq_number;
				} else {
					/* Timestamp of another packet, stop here after we've decoded the previous one */
					pkt = NULL;
					packet = NULL;
					bytes = 0;
					ts_changed = TRUE;
					prevts_set = FALSE;
					JANUS_LOG(LOG_HUGE, "[%s]   >> Got new video timestamp (%"SCNu32" != %"SCNu32"), stopping here\n",
						session->ndi_name, ntohl(rtp->timestamp), last_ts);
				}
				/* FIXME Check if the timestamp changed and we need to decode */
				if(got_video && ts_changed && frame_len == 0) {
					ts_changed = FALSE;
				} else if(got_video && ts_changed && frame_len > 0) {
					/* Timestamp changed: we have a whole packet to decode */
					ts_changed = FALSE;
					JANUS_LOG(LOG_HUGE, "[%s]   >> Decoding video frame: ts=%"SCNu32"\n",
						session->ndi_name, last_ts);
					/* FIXME Do we have gaps in this packet? */
					if(gaps > 0) {
						/* Should we stop here, or just show a warning? */
						JANUS_LOG(LOG_WARN, "[%s] We're missing at least %"SCNu8" packets in this frame, skipping it\n",
							session->ndi_name, gaps);
						if(got_keyframe) {
							/* Wait for a keyframe */
							waiting_kf = TRUE;
							need_pli = TRUE;
						}
						/* Reset the offset and stop here */
						frame_len = 0;
						data_len = 0;
						janus_ndi_buffer_packet_destroy(pkt);
						break;
					}
					if(got_keyframe && waiting_kf && !key_frame) {
						/* We're waiting for a keyframe from a previous glitch */
						JANUS_LOG(LOG_WARN, "[%s] Still waiting for a keyframe to fix the glitch\n", session->ndi_name);
						/* Reset the offset and stop here */
						frame_len = 0;
						data_len = 0;
						janus_ndi_buffer_packet_destroy(pkt);
						break;
					}
					if(data_len > 0) {
						/* AV1 only: we have a buffered OBU, write the OBU size */
						size_t written = 0;
						uint8_t leb[8];
						janus_ndi_av1_lev128_encode(data_len, leb, &written);
						JANUS_LOG(LOG_HUGE, "[%s] OBU size (%d): %zu\n", session->ndi_name, data_len, written);
						memcpy(received_frame + frame_len, leb, written);
						frame_len += written;
						/* Copy the actual data */
						JANUS_LOG(LOG_HUGE, "[%s] OBU data: %"SCNu32"\n", session->ndi_name, data_len);
						memcpy(received_frame + frame_len, obu_data, data_len);
						frame_len += data_len;
					}
					memset(received_frame + frame_len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
					AVPacket avpacket = { 0 };
					avpacket.data = received_frame;
					avpacket.size = frame_len;
					if(got_keyframe) {
						if(key_frame) {
							avpacket.flags |= AV_PKT_FLAG_KEY;
							key_frame = FALSE;
							waiting_kf = FALSE;
						}
						/* We only start decoding after we received the first keyframe */
						int ret = avcodec_send_packet(session->ctx, &avpacket);
						if(ret < 0) {
							JANUS_LOG(LOG_ERR, "[%s] Error decoding video frame... %d (%s)\n",
								session->ndi_name, ret, av_err2str(ret));
							/* Schedule a PLI */
							need_pli = TRUE;
						} else {
							ret = avcodec_receive_frame(session->ctx, decoded_frame);
							if(ret == AVERROR(EAGAIN)) {
								/* Encoder needs more input? */
								JANUS_LOG(LOG_VERB, "[%s] Skipping decoding of video frame: %d (%s)\n",
									session->ndi_name, ret, av_err2str(ret));
							} else if(ret < 0) {
								JANUS_LOG(LOG_ERR, "[%s] Error decoding video frame: %d (%s)\n",
									session->ndi_name, ret, av_err2str(ret));
								/* Schedule a PLI */
								need_pli = TRUE;
							}
						}
						frame = decoded_frame;
						if(ret == 0) {
							need_pli = FALSE;
							JANUS_LOG(LOG_HUGE, "[%s] Decoded video frame: %dx%d\n",
								session->ndi_name, frame->width, frame->height);
							if(!g_atomic_int_get(&session->video) || g_atomic_int_get(&session->paused)) {
								/* NDI translation is paused, skip this frame */
								frame_len = 0;
								data_len = 0;
								janus_ndi_buffer_packet_destroy(pkt);
								continue;
							}
							/* Do we need to (re)create the scalers? */
							if(sws == NULL || frame->width != session->width || frame->height != session->height) {
								/* We do: get rid of the old ones, if any, and recreate them all */
								session->width = frame->width;
								session->height = frame->height;
								int target_width = session->target_width ? session->target_width : session->width;
								int target_height = session->target_height ? session->target_height : session->height;
								/* Create the scaler(s) */
								JANUS_LOG(LOG_INFO, "[%s] Creating scaler: %dx%d (YUV) --> %dx%d (UYVY)\n",
									session->ndi_name, frame->width, frame->height, target_width, target_height);
								if(sws)
									sws_freeContext(sws);
								sws = sws_getContext(sws_canvas ? target_width : frame->width, sws_canvas ? target_height : frame->height, AV_PIX_FMT_YUV420P,
									target_width, target_height, AV_PIX_FMT_UYVY422, SWS_FAST_BILINEAR, NULL, NULL, NULL);
								if(sws == NULL) {
									/* TODO What should we do?? */
									JANUS_LOG(LOG_WARN, "[%s] Couldn't initialize scaler...\n", session->ndi_name);
									frame_len = 0;
									data_len = 0;
									janus_ndi_buffer_packet_destroy(pkt);
									continue;
								}
								/* Recreate the scaled frame too */
								if(scaled_frame != NULL) {
									av_free(scaled_frame->data[0]);
									av_frame_free(&scaled_frame);
								}
								scaled_frame = av_frame_alloc();
								scaled_frame->width = target_width;
								scaled_frame->height = target_height;
								scaled_frame->format = AV_PIX_FMT_UYVY422;
								ret = av_image_alloc(scaled_frame->data, scaled_frame->linesize,
									scaled_frame->width, scaled_frame->height, AV_PIX_FMT_UYVY422, 1);
								if(ret < 0) {
									JANUS_LOG(LOG_WARN, "[%s] Error allocating frame buffer: %d (%s)\n",
										session->ndi_name, ret, av_err2str(ret));
									frame_len = 0;
									data_len = 0;
									janus_ndi_buffer_packet_destroy(pkt);
									continue;
								}
							}
							/* Convert the frame to the format we need */
							sws_scale(sws, (const uint8_t * const*)(canvas ? canvas->data : frame->data), canvas ? canvas->linesize : frame->linesize,
								0, canvas ? canvas->height : frame->height, scaled_frame->data, scaled_frame->linesize);
							/* Send via NDI */
							NDIlib_video_frame_v2_t NDI_video_frame = { 0 };
							NDI_video_frame.xres = scaled_frame->width;
							NDI_video_frame.yres = scaled_frame->height;
							NDI_video_frame.FourCC = NDIlib_FourCC_type_UYVY;
							NDI_video_frame.p_data = scaled_frame->data[0];
							NDI_video_frame.line_stride_in_bytes = scaled_frame->linesize[0];
							NDI_video_frame.timecode = NDIlib_send_timecode_synthesize;
							NDI_video_frame.frame_format_type = NDIlib_frame_format_type_progressive;
							janus_mutex_lock(&session->ndi_sender->mutex);
							if(session->fps > 0) {
								NDI_video_frame.frame_rate_D = 1;
								NDI_video_frame.frame_rate_N = session->fps;
							}
							session->ndi_sender->last_updated = janus_get_monotonic_time();
							NDIlib_send_send_video_v2(session->ndi_sender->instance, &NDI_video_frame);
							janus_mutex_unlock(&session->ndi_sender->mutex);
						}
					}
					/* Reset the offset and stop here */
					frame_len = 0;
					data_len = 0;
					janus_ndi_buffer_packet_destroy(pkt);
					continue;
				}
				if(packet == NULL) {
					janus_ndi_buffer_packet_destroy(pkt);
					continue;
				}
				got_video = TRUE;
				/* Check what needs to be skipped before getting to the payload */
				payload = janus_rtp_payload(packet, bytes, &plen);
				if(!payload || plen < 1) {
					/* Nothing to do here */
					JANUS_LOG(LOG_VERB, "[%s] Nothing to decode (%d bytes)\n",
						session->ndi_name, plen);
					/* Get rid of the buffered packet */
					janus_ndi_buffer_packet_destroy(pkt);
					continue;
				}
				if(session->vcodec == JANUS_VIDEOCODEC_VP8) {
					/* VP8 depay */
					JANUS_LOG(LOG_HUGE, "[%s]   -- Video packet (VP8)\n", session->ndi_name);
					/* Read the first octet (VP8 Payload Descriptor) */
					char *buffer = payload;
					bytes = plen-1;
					uint8_t vp8pd = *buffer;
					uint8_t xbit = (vp8pd & 0x80);
					uint8_t sbit = (vp8pd & 0x10);
					/* Read the Extended control bits octet */
					if(xbit) {
						buffer++;
						bytes--;
						vp8pd = *buffer;
						uint8_t ibit = (vp8pd & 0x80);
						uint8_t lbit = (vp8pd & 0x40);
						uint8_t tbit = (vp8pd & 0x20);
						uint8_t kbit = (vp8pd & 0x10);
						if(ibit) {
							/* Read the PictureID octet */
							buffer++;
							bytes--;
							vp8pd = *buffer;
							uint16_t picid = vp8pd, wholepicid = picid;
							uint8_t mbit = (vp8pd & 0x80);
							if(mbit) {
								memcpy(&picid, buffer, sizeof(uint16_t));
								wholepicid = ntohs(picid);
								picid = (wholepicid & 0x7FFF);
								buffer++;
								bytes--;
							}
						}
						if(lbit) {
							/* Read the TL0PICIDX octet */
							buffer++;
							bytes--;
							vp8pd = *buffer;
						}
						if(tbit || kbit) {
							/* Read the TID/KEYIDX octet */
							buffer++;
							bytes--;
							vp8pd = *buffer;
						}
					}
					buffer++;
					if(sbit) {
						unsigned long int vp8ph = 0;
						memcpy(&vp8ph, buffer, 4);
						vp8ph = ntohl(vp8ph);
						uint8_t pbit = ((vp8ph & 0x01000000) >> 24);
						if(!pbit) {
							/* Get resolution */
							unsigned char *c = (unsigned char *)buffer+3;
							/* vet via sync code */
							if(c[0]!=0x9d||c[1]!=0x01||c[2]!=0x2a) {
								JANUS_LOG(LOG_WARN, "[%s] First 3-bytes after header not what they're supposed to be?\n",
									session->ndi_name);
							} else {
								key_frame = TRUE;
								if(!got_keyframe)
									got_keyframe = TRUE;
								uint16_t val3, val5;
								memcpy(&val3, c+3, sizeof(uint16_t));
								int vp8w = swap2(val3)&0x3fff;
								memcpy(&val5, c+5, sizeof(uint16_t));
								int vp8h = swap2(val5)&0x3fff;
								/* Check if the resolution is different than the one we knew... */
								if(width != vp8w || height != vp8h) {
									/* It is: take note of the new resolution */
									JANUS_LOG(LOG_INFO, "[%s] VP8 resolution changed (was %dx%d, now is %dx%d)\n",
										session->ndi_name, width, height, vp8w, vp8h);
									width = vp8w;
									height = vp8h;
								}
							}
						}
					}
					/* Frame manipulation: append the actual payload to the buffer */
					if(bytes > 0) {
						if(frame_len + bytes > canvas_size) {
							JANUS_LOG(LOG_WARN, "[%s] Frame exceeds buffer size...\n",
								session->ndi_name);
						} else {
							memcpy(received_frame + frame_len, buffer, bytes);
							frame_len += bytes;
						}
					}
				} else if(session->vcodec == JANUS_VIDEOCODEC_VP9) {
					/* VP9 depay */
					JANUS_LOG(LOG_HUGE, "[%s]   -- Video packet (VP9)\n", session->ndi_name);
					/* Read the first octet (VP9 Payload Descriptor) */
					char *buffer = payload;
					bytes = plen;
					uint8_t vp9pd = *buffer;
					uint8_t ibit = (vp9pd & 0x80);
					uint8_t pbit = (vp9pd & 0x40);
					uint8_t lbit = (vp9pd & 0x20);
					uint8_t fbit = (vp9pd & 0x10);
					uint8_t vbit = (vp9pd & 0x02);
					/* Move to the next octet and see what's there */
					buffer++;
					bytes--;
					if(ibit) {
						/* Read the PictureID octet */
						vp9pd = *buffer;
						uint16_t picid = vp9pd, wholepicid = picid;
						uint8_t mbit = (vp9pd & 0x80);
						if(!mbit) {
							buffer++;
							bytes--;
						} else {
							memcpy(&picid, buffer, sizeof(uint16_t));
							wholepicid = ntohs(picid);
							picid = (wholepicid & 0x7FFF);
							buffer += 2;
							bytes -= 2;
						}
					}
					if(lbit) {
						buffer++;
						bytes--;
						if(!fbit) {
							/* Non-flexible mode, skip TL0PICIDX */
							buffer++;
							bytes--;
						}
					}
					if(fbit && pbit) {
						/* Skip reference indices */
						uint8_t nbit = 1;
						while(nbit) {
							vp9pd = *buffer;
							nbit = (vp9pd & 0x01);
							buffer++;
							bytes--;
						}
					}
					if(vbit) {
						/* Parse and skip SS */
						vp9pd = *buffer;
						int n_s = (vp9pd & 0xE0) >> 5;
						n_s++;
						uint8_t ybit = (vp9pd & 0x10);
						uint8_t gbit = (vp9pd & 0x08);
						if(ybit) {
							/* Iterate on all spatial layers and get resolution */
							buffer++;
							bytes--;
							int i=0;
							for(i=0; i<n_s; i++) {
								/* Width */
								uint16_t w;
								memcpy(&w, buffer, sizeof(uint16_t));
								int vp9w = ntohs(w);
								buffer += 2;
								/* Height */
								uint16_t h;
								memcpy(&h, buffer, sizeof(uint16_t));
								int vp9h = ntohs(h);
								buffer += 2;
								bytes -= 4;
								/* Check if the resolution is different than the one we knew... */
								if(width != vp9w || height != vp9h) {
									/* It is: take note of the new resolution */
									JANUS_LOG(LOG_INFO, "[%s] VP9 resolution changed (was %dx%d, now is %dx%d)\n",
										session->ndi_name, width, height, vp9w, vp9h);
									width = vp9w;
									height = vp9h;
								}
								key_frame = TRUE;
								if(!got_keyframe)
									got_keyframe = TRUE;
							}
						}
						if(gbit) {
							if(!ybit) {
								buffer++;
								bytes--;
							}
							uint8_t n_g = *buffer;
							buffer++;
							bytes--;
							if(n_g > 0) {
								uint i=0;
								for(i=0; i<n_g; i++) {
									/* Read the R bits */
									vp9pd = *buffer;
									int r = (vp9pd & 0x0C) >> 2;
									if(r > 0) {
										/* Skip reference indices */
										buffer += r;
										bytes -= r;
									}
									buffer++;
									bytes--;
								}
							}
						}
					}
					/* Frame manipulation: append the actual payload to the buffer */
					if(bytes > 0) {
						if(frame_len + bytes > canvas_size) {
							JANUS_LOG(LOG_WARN, "[%s] Frame exceeds buffer size...\n",
								session->ndi_name);
						} else {
							memcpy(received_frame + frame_len, buffer, bytes);
							frame_len += bytes;
						}
					}
				} else if(session->vcodec == JANUS_VIDEOCODEC_H264) {
					/* H.264 depay */
					JANUS_LOG(LOG_HUGE, "[%s]   -- Video packet (H.264)\n", session->ndi_name);
					char *buffer = payload;
					int len = plen, jump = 0;
					uint8_t fragment = *buffer & 0x1F;
					uint8_t nal = *(buffer+1) & 0x1F;
					uint8_t start_bit = *(buffer+1) & 0x80;
					if(fragment == 7) {
						/* SPS, see if we can extract the width/height as well */
						int h264w = 0, h264h = 0;
						janus_ndi_h264_parse_sps(buffer, &h264w, &h264h);
						if(width != h264w || height != h264h) {
							/* It is: take note of the new resolution */
							JANUS_LOG(LOG_INFO, "[%s] H.264 resolution changed (was %dx%d, now is %dx%d)\n",
								session->ndi_name, width, height, h264w, h264h);
							width = h264w;
							height = h264h;
						}
					} else if(fragment == 24) {
						/* May we find an SPS in this STAP-A? */
						char *temp = buffer;
						temp++;
						int tot = len-1;
						uint16_t psize = 0;
						while(tot > 0) {
							memcpy(&psize, buffer, 2);
							psize = ntohs(psize);
							temp += 2;
							tot -= 2;
							int nal = *temp & 0x1F;
							if(nal == 7) {
								int h264w = 0, h264h = 0;
								janus_ndi_h264_parse_sps(temp, &h264w, &h264h);
								if(width != h264w || height != h264h) {
									/* It is: take note of the new resolution */
									JANUS_LOG(LOG_INFO, "[%s] H.264 resolution changed (was %dx%d, now is %dx%d)\n",
										session->ndi_name, width, height, h264w, h264h);
									width = h264w;
									height = h264h;
								}
							}
							temp += psize;
							tot -= psize;
						}
					}
					if(fragment == 28 || fragment == 29) {
						JANUS_LOG(LOG_HUGE, "[%s] Fragment=%d, NAL=%d, Start=%d (len=%d, frame_len=%d)\n",
							session->ndi_name, fragment, nal, start_bit, len, frame_len);
					} else {
						JANUS_LOG(LOG_HUGE, "[%s] Fragment=%d (len=%d, frame_len=%d)\n",
							session->ndi_name, fragment, len, frame_len);
					}
					if(fragment == 5 ||
							((fragment == 28 || fragment == 29) && nal == 5 && start_bit == 128)) {
						JANUS_LOG(LOG_VERB, "[%s] (seq=%"SCNu16", ts=%"SCNu32") Key frame\n",
							session->ndi_name, ntohs(rtp->seq_number), ntohl(rtp->timestamp));
						key_frame = TRUE;
						if(!got_keyframe)
							got_keyframe = TRUE;
					}
					/* Frame manipulation */
					if((fragment > 0) && (fragment < 24)) {	/* Add a start code */
						uint8_t *temp = received_frame + frame_len;
						memset(temp, 0x00, 1);
						memset(temp + 1, 0x00, 1);
						memset(temp + 2, 0x01, 1);
						frame_len += 3;
					} else if(fragment == 24) {	/* STAP-A */
						/* De-aggregate the NALs and write each of them separately */
						buffer++;
						int tot = len-1;
						uint16_t psize = 0;
						frame_len = 0;
						while(tot > 0) {
							memcpy(&psize, buffer, 2);
							psize = ntohs(psize);
							buffer += 2;
							tot -= 2;
							/* Now we have a single NAL */
							uint8_t *temp = received_frame + frame_len;
							memset(temp, 0x00, 1);
							memset(temp + 1, 0x00, 1);
							memset(temp + 2, 0x01, 1);
							frame_len += 3;
							memcpy(received_frame + frame_len, buffer, psize);
							frame_len += psize;
							/* Go on */
							buffer += psize;
							tot -= psize;
						}
						len = tot;
					} else if((fragment == 28) || (fragment == 29)) {	/* FIXME true fr FU-A, not FU-B */
						uint8_t indicator = *buffer;
						uint8_t header = *(buffer+1);
						jump = 2;
						len -= 2;
						if(header & 0x80) {
							/* First part of fragmented packet (S bit set) */
							uint8_t *temp = received_frame + frame_len;
							memset(temp, 0x00, 1);
							memset(temp + 1, 0x00, 1);
							memset(temp + 2, 0x01, 1);
							memset(temp + 3, (indicator & 0xE0) | (header & 0x1F), 1);
							frame_len += 4;
						} else if (header & 0x40) {
							/* Last part of fragmented packet (E bit set) */
						}
					}
					/* Frame manipulation: append the actual payload to the buffer */
					if(len > 0) {
						if(frame_len + len > canvas_size) {
							JANUS_LOG(LOG_WARN, "[%s] Frame exceeds buffer size...\n", session->ndi_name);
						} else {
							memcpy(received_frame + frame_len, buffer+jump, len);
							frame_len += len;
						}
					}
				} else if(session->vcodec == JANUS_VIDEOCODEC_AV1) {
					/* AV1 depay */
					JANUS_LOG(LOG_HUGE, "[%s]   -- Video packet (AV1)\n", session->ndi_name);
					char *buffer = payload;
					int len = plen;
					uint8_t aggrh = *buffer;
					uint8_t zbit = (aggrh & 0x80) >> 7;
					uint8_t ybit = (aggrh & 0x40) >> 6;
					uint8_t w = (aggrh & 0x30) >> 4;
					uint8_t nbit = (aggrh & 0x08) >> 3;
					JANUS_LOG(LOG_HUGE, "[%s]  -- OBU aggregation header: z=%u, y=%u, w=%u, n=%u\n",
						session->ndi_name, zbit, ybit, w, nbit);
					/* FIXME Ugly hack: we consider a packet with Z=0 and N=1 a keyframe */
					key_frame = (!zbit && nbit);
					if(key_frame && !got_keyframe)
						got_keyframe = TRUE;
					buffer++;
					len--;
					uint8_t obus = 0;
					uint32_t obusize = 0;
					while(!zbit && len > 0) {
						obus++;
						if(w == 0 || w > obus) {
							/* Read the OBU size (leb128) */
							size_t read = 0;
							obusize = janus_ndi_av1_lev128_decode((uint8_t *)buffer, len, &read);
							buffer += read;
							len -= read;
						} else {
							obusize = len;
						}
						/* Then we have the OBU header */
						char *payload = buffer;
						uint8_t obuh = *payload;
						uint8_t fbit = (obuh & 0x80) >> 7;
						uint8_t type = (obuh & 0x78) >> 3;
						uint8_t ebit = (obuh & 0x04) >> 2;
						uint8_t sbit = (obuh & 0x02) >> 1;
						JANUS_LOG(LOG_HUGE, "[%s]  -- OBU header: f=%u, type=%u, e=%u, s=%u\n",
							session->ndi_name, fbit, type, ebit, sbit);
						if(ebit) {
							/* Skip the extension, if present */
							payload++;
							len--;
							obusize--;
						}
						if(type == 1) {
							/* Sequence header */
							uint16_t av1w = 0, av1h = 0;
							/* TODO Fix currently broken parsing of SH */
							janus_ndi_av1_parse_sh(payload+1, &av1w, &av1h);
							if(width != av1w || height != av1h) {
								/* It is: take note of the new resolution */
								JANUS_LOG(LOG_INFO, "[%s] AV1 resolution changed (was %dx%d, now is %dx%d)\n",
									session->ndi_name, width, height, av1w, av1h);
								width = av1w;
								height = av1h;
							}
						}
						/* Update the OBU header to set the S bit */
						obuh = *buffer;
						obuh |= (1 << 1);
						JANUS_LOG(LOG_HUGE, "[%s] OBU header: 1\n", session->ndi_name);
						memcpy(received_frame + frame_len, &obuh, sizeof(uint8_t));
						frame_len++;
						buffer++;
						len--;
						obusize--;
						if(w == 0 || w > obus || !ybit) {
							/* We have the whole OBU, write the OBU size */
							size_t written = 0;
							uint8_t leb[8];
							janus_ndi_av1_lev128_encode(obusize, leb, &written);
							JANUS_LOG(LOG_HUGE, "[%s] OBU size (%"SCNu32"): %zu\n", session->ndi_name, obusize, written);
							memcpy(received_frame + frame_len, leb, written);
							frame_len += written;
							/* Copy the actual data */
							JANUS_LOG(LOG_HUGE, "[%s] OBU data: %"SCNu32"\n", session->ndi_name, obusize);
							memcpy(received_frame + frame_len, buffer, obusize);
							frame_len += obusize;
						} else {
							/* OBU will continue in another packet, buffer the data */
							JANUS_LOG(LOG_HUGE, "[%s] OBU data (part.): %d\n", session->ndi_name, obusize);
							memcpy(obu_data + data_len, buffer, obusize);
							data_len += obusize;
						}
						/* Move to the next OBU, if any */
						buffer += obusize;
						len -= obusize;
					}
					/* Frame manipulation */
					if(data_len > 0) {
						if(frame_len + len > canvas_size) {
							JANUS_LOG(LOG_WARN, "[%s] Frame exceeds buffer size...\n", session->ndi_name);
						} else {
							JANUS_LOG(LOG_HUGE, "[%s] OBU data (cont.): %d\n", session->ndi_name, len);
							memcpy(obu_data + data_len, buffer, len);
							data_len += len;
						}
					}
				}
				/* Get rid of the buffered packet */
				janus_ndi_buffer_packet_destroy(pkt);
			}
		}
	}

	/* Also notify event handlers */
	if(notify_events && gateway->events_is_enabled()) {
		json_t *info = json_object();
		json_object_set_new(info, "name", json_string(session->ndi_name));
		json_object_set_new(info, "event", json_string("pausing"));
		gateway->notify_event(&janus_ndi_plugin, NULL, info);
	}

	/* In case there's no external sender, send a black box as the last frame */
	if(!session->external_sender && session->disconnected && scaled_frame != NULL) {
		/* Generate a disconnected image as large as the last frame we sent */
		int width = scaled_frame->width;
		int height = scaled_frame->height;
		AVFrame *goodbye = janus_ndi_generate_disconnected_image(session->disconnected,
			session->disconnected_color ? session->disconnected_color : "000000", width, height);
		if(goodbye != NULL) {
			/* Send via NDI */
			NDIlib_video_frame_v2_t NDI_video_frame = { 0 };
			NDI_video_frame.xres = goodbye->width;
			NDI_video_frame.yres = goodbye->height;
			NDI_video_frame.FourCC = NDIlib_FourCC_type_UYVY;
			NDI_video_frame.p_data = goodbye->data[0];
			NDI_video_frame.line_stride_in_bytes = goodbye->linesize[0];
			NDI_video_frame.timecode = NDIlib_send_timecode_synthesize;
			janus_mutex_lock(&session->ndi_sender->mutex);
			session->ndi_sender->last_updated = janus_get_monotonic_time();
			NDIlib_send_send_video_v2(session->ndi_sender->instance, &NDI_video_frame);
			janus_mutex_unlock(&session->ndi_sender->mutex);
			/* Destroy the frame */
			av_free(goodbye->data[0]);
			av_frame_free(&goodbye);
			/* Give the frame time to be sent before we destroy the sender */
			g_usleep(10000);
		}
	}

	/* Cleanup resources */
	g_free(received_frame);
	av_frame_free(&decoded_frame);
	if(scaled_frame != NULL) {
		av_free(scaled_frame->data[0]);
		av_frame_free(&scaled_frame);
	}
	if(sws)
		sws_freeContext(sws);
	if(sws_canvas)
		sws_freeContext(sws_canvas);

	/* Get rid of the NDI sender */
	janus_mutex_lock(&sessions_mutex);
	if(session->ndi_sender != NULL) {
		if(session->ndi_sender->placeholder) {
			/* Restore the placeholder */
			janus_mutex_lock(&session->ndi_sender->mutex);
			NDIlib_send_clear_connection_metadata(session->ndi_sender->instance);
			if(session->ndi_sender->metadata != NULL) {
				NDIlib_metadata_frame_t NDI_product_type;
				NDI_product_type.p_data = session->ndi_sender->metadata;
				NDIlib_send_add_connection_metadata(session->ndi_sender->instance, &NDI_product_type);
			}
			/* Done */
			session->ndi_sender->busy = FALSE;
			janus_mutex_unlock(&session->ndi_sender->mutex);
			janus_refcount_decrease(&session->ndi_sender->ref);
			janus_refcount_decrease(&session->ref);
		} else {
			g_hash_table_remove(ndi_names, session->ndi_name);
		}
	}
	session->ndi_sender = NULL;
	janus_mutex_unlock(&sessions_mutex);
	/* Get rid of the SDP */
	janus_sdp_destroy(session->sdp);
	session->sdp = NULL;
	/* Get rid of the decoders */
	if(session->audiodec) {
		opus_decoder_destroy(session->audiodec);
		session->audiodec = NULL;
	}
	if(session->ctx) {
		avcodec_free_context(&session->ctx);
		av_free(session->ctx);
		session->ctx = NULL;
	}
	if(canvas != NULL) {
		av_free(canvas->data[0]);
		av_frame_free(&canvas);
	}

	JANUS_LOG(LOG_INFO, "[%s] Leaving session thread\n", session->ndi_name);

	/* Stop tracking the name */
	g_free(session->ndi_name);
	session->ndi_name = NULL;

	/* Remove the reference to the session that the thread had */
	janus_refcount_decrease(&session->ref);

	g_thread_unref(g_thread_self());
	return NULL;
}

static void *janus_ndi_send_test_pattern(void *data) {
	JANUS_LOG(LOG_INFO, "Sending test pattern: %s\n", test_pattern_name);

	/* Create NDI sender */
	NDIlib_send_create_t NDI_send_create_desc = {0};
	NDI_send_create_desc.p_ndi_name = test_pattern_name;
	NDIlib_send_instance_t ndi_sender = NDIlib_send_create(&NDI_send_create_desc);
	if(ndi_sender == NULL) {
		JANUS_LOG(LOG_ERR, "Error creating NDI source for test pattern\n");
		g_atomic_int_set(&test_pattern_running, 0);
		g_thread_unref(g_thread_self());
		return NULL;
	}

	/* We send the pattern at ~10fps */
	int fps = 10;
	struct timeval now, before;
	gettimeofday(&before, NULL);
	now.tv_sec = before.tv_sec;
	now.tv_usec = before.tv_usec;
	time_t passed, d_s, d_us;

	while(g_atomic_int_get(&test_pattern_running) != -1) {
		/* See if it's time to send the pattern */
		gettimeofday(&now, NULL);
		d_s = now.tv_sec - before.tv_sec;
		d_us = now.tv_usec - before.tv_usec;
		if(d_us < 0) {
			d_us += 1000000;
			--d_s;
		}
		passed = d_s*1000000 + d_us;
		if(passed < (1000000000/(fps*2000))) {
			usleep(1000);
			continue;
		}
		/* Update the reference time */
		before.tv_usec += 1000000000/(fps*1000);
		if(before.tv_usec > 1000000) {
			before.tv_sec++;
			before.tv_usec -= 1000000;
		}

		/* Send via NDI */
		NDIlib_video_frame_v2_t NDI_video_frame = { 0 };
		NDI_video_frame.xres = test_pattern->width;
		NDI_video_frame.yres = test_pattern->height;
		NDI_video_frame.FourCC = NDIlib_FourCC_type_UYVY;
		NDI_video_frame.p_data = test_pattern->data[0];
		NDI_video_frame.line_stride_in_bytes = test_pattern->linesize[0];
		NDI_video_frame.timecode = NDIlib_send_timecode_synthesize;
		NDI_video_frame.frame_format_type = NDIlib_frame_format_type_progressive;
		NDI_video_frame.frame_rate_D = 1;
		NDI_video_frame.frame_rate_N = 30;
		NDIlib_send_send_video_v2(ndi_sender, &NDI_video_frame);
	}

	JANUS_LOG(LOG_INFO, "Stopping test pattern: %s\n", test_pattern_name);
	NDIlib_send_destroy(ndi_sender);

	g_atomic_int_set(&test_pattern_running, 0);
	test_pattern_thread = NULL;

	g_thread_unref(g_thread_self());
	return NULL;
}

static void *janus_ndi_placeholder_thread(void *data) {
	janus_ndi_sender *sender = (janus_ndi_sender *)data;
	if(!sender) {
		JANUS_LOG(LOG_ERR, "Invalid sender, leaving thread...\n");
		g_thread_unref(g_thread_self());
		return NULL;
	}
	JANUS_LOG(LOG_INFO, "[%s] Starting NDI sender thread\n", sender->name);

	/* We send the placeholder at 30fps, when the sender isn't busy or active */
	int fps = 30;
	struct timeval now, before;
	gettimeofday(&before, NULL);
	now.tv_sec = before.tv_sec;
	now.tv_usec = before.tv_usec;
	time_t passed, d_s, d_us;
	gint64 nowm = 0;

	while(!g_atomic_int_get(&sender->destroyed)) {
		/* See if it's time to do something */
		gettimeofday(&now, NULL);
		d_s = now.tv_sec - before.tv_sec;
		d_us = now.tv_usec - before.tv_usec;
		if(d_us < 0) {
			d_us += 1000000;
			--d_s;
		}
		passed = d_s*1000000 + d_us;
		if(passed < (1000000000/(fps*2000))) {
			usleep(1000);
			continue;
		}
		/* Update the reference time */
		before.tv_usec += 1000000000/(fps*1000);
		if(before.tv_usec > 1000000) {
			before.tv_sec++;
			before.tv_usec -= 1000000;
		}
		janus_mutex_lock(&sender->mutex);
		nowm = janus_get_monotonic_time();
		if(nowm < sender->last_updated || (nowm - sender->last_updated < 500000)) {
			/* Sender is busy, retry later */
			janus_mutex_unlock(&sender->mutex);
			continue;
		}
		/* Send via NDI */
		NDIlib_video_frame_v2_t NDI_video_frame = { 0 };
		NDI_video_frame.xres = sender->image->width;
		NDI_video_frame.yres = sender->image->height;
		NDI_video_frame.FourCC = NDIlib_FourCC_type_UYVY;
		NDI_video_frame.p_data = sender->image->data[0];
		NDI_video_frame.line_stride_in_bytes = sender->image->linesize[0];
		NDI_video_frame.timecode = NDIlib_send_timecode_synthesize;
		NDI_video_frame.frame_format_type = NDIlib_frame_format_type_progressive;
		NDI_video_frame.frame_rate_D = 1;
		NDI_video_frame.frame_rate_N = 30;
		NDIlib_send_send_video_v2(sender->instance, &NDI_video_frame);
		janus_mutex_unlock(&sender->mutex);
	}

	JANUS_LOG(LOG_INFO, "[%s] Stopping NDI sender thread\n", sender->name);
	janus_refcount_decrease(&sender->ref);

	g_thread_unref(g_thread_self());
	return NULL;
}

/* Helpers to download an image and decode it to an AVFrame (for placeholders) */
static AVFrame *janus_ndi_download_image(const char *path) {
	if(path == NULL)
		return NULL;
	/* Do we have a frame already for this path? */
	janus_mutex_lock(&img_mutex);
	AVFrame *frame = g_hash_table_lookup(images, path);
	if(frame != NULL) {
		janus_mutex_unlock(&img_mutex);
		JANUS_LOG(LOG_VERB, "Already downloaded and decoded: %s\n", path);
		return frame;
	}
	/* Is this a web link or a local file path? */
	char filename[255];
	if(strstr(path, "file://") == path) {
		/* Local file */
		if(!strcmp(path, "file://")) {
			janus_mutex_unlock(&img_mutex);
			JANUS_LOG(LOG_ERR, "Couldn't open file: %s\n", path);
			return NULL;
		}
		/* Skip the file:// part */
		path += 7;
		g_snprintf(filename, sizeof(filename), "%s", path);
		goto process;
	}
	/* Web link, we need to download it */
	JANUS_LOG(LOG_VERB, "Sending GET request: %s\n", path);
	g_snprintf(filename, sizeof(filename), "/tmp/%"SCNu32".jnp", g_random_int());
	JANUS_LOG(LOG_VERB, "  -- Will save to file: %s\n", filename);
	FILE *file = fopen(filename, "wb");
	if(file == NULL) {
		janus_mutex_unlock(&img_mutex);
		JANUS_LOG(LOG_ERR, "Couldn't open file: %s\n", filename);
		return NULL;
	}
	/* Prepare the libcurl context */
	CURLcode res;
	CURL *curl = curl_easy_init();
	if(curl == NULL) {
		janus_mutex_unlock(&img_mutex);
		fclose(file);
		JANUS_LOG(LOG_ERR, "libcurl error\n");
		return NULL;
	}
	curl_easy_setopt(curl, CURLOPT_URL, path);
	curl_easy_setopt(curl, CURLOPT_HTTPGET, 1);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);	/* FIXME Max 10 seconds */
	/* For getting data, we use an helper struct and the libcurl callback */
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)file);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "JanusNDIPlugin/1.0");
	/* Send the request */
	res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	if(res != CURLE_OK) {
		janus_mutex_unlock(&img_mutex);
		fclose(file);
		JANUS_LOG(LOG_ERR, "Couldn't send the request: %s\n", curl_easy_strerror(res));
		return NULL;
	}
	/* Process the response */
	size_t fsize = ftell(file);
	fclose(file);
	JANUS_LOG(LOG_VERB, "Downloaded image: %zu bytes (%s)\n", fsize, path);

process:
	frame = janus_ndi_decode_image(filename);
	if(frame)
		g_hash_table_insert(images, g_strdup(path), frame);
	/* Done */
	janus_mutex_unlock(&img_mutex);
	return frame;
}

static AVFrame *janus_ndi_decode_image(char *filename) {
	AVFormatContext *fctx = NULL;
	int err = 0;
	err = avformat_open_input(&fctx, filename, NULL, NULL);
	if(err != 0) {
		JANUS_LOG(LOG_ERR, "Couldn't open the image file (%s)... %d (%s)\n", filename, err, av_err2str(err));
		return NULL;
	}
	if(fctx->nb_streams < 1) {
		JANUS_LOG(LOG_ERR, "No stream available for the image file (%s)...\n", filename);
		avformat_close_input(&fctx);
		avformat_free_context(fctx);
		return NULL;
	}
	AVCodecParameters *codecpar = fctx->streams[0]->codecpar;
	const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
	if(!codec) {
		JANUS_LOG(LOG_ERR, "Couldn't find the decoder for the image file (%s)...\n", filename);
		avformat_close_input(&fctx);
		avformat_free_context(fctx);
		return NULL;
	}
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(ctx, codecpar);
	err = avcodec_open2(ctx, codec, NULL);
	if(err < 0) {
		JANUS_LOG(LOG_ERR, "Couldn't initiate the codec to open the image file (%s)... %d (%s)\n",
			filename, err, av_err2str(err));
		avcodec_free_context(&ctx);
		avformat_close_input(&fctx);
		avformat_free_context(fctx);
		return NULL;
	}
	AVPacket packet = { 0 };
	err = av_read_frame(fctx, &packet);
	if(err < 0) {
		JANUS_LOG(LOG_ERR, "Couldn't get a valid packet from the image file '%s': %d (%s)\n",
			filename, err, av_err2str(err));
		avcodec_free_context(&ctx);
		avformat_close_input(&fctx);
		avformat_free_context(fctx);
		av_packet_unref(&packet);
		return NULL;
	}
	AVFrame *bgRGB = av_frame_alloc();
	err = avcodec_send_packet(ctx, &packet);
	if(err < 0) {
		JANUS_LOG(LOG_ERR, "Image NOT loaded: %d (%s)\n", err, av_err2str(err));
		avcodec_free_context(&ctx);
		avformat_close_input(&fctx);
		avformat_free_context(fctx);
		av_frame_free(&bgRGB);
		av_packet_unref(&packet);
		return NULL;
	}
	av_packet_unref(&packet);
	err = avcodec_receive_frame(ctx, bgRGB);
	if(err < 0) {
		JANUS_LOG(LOG_ERR, "Image NOT loaded: %d (%s)\n", err, av_err2str(err));
		avcodec_free_context(&ctx);
		avformat_close_input(&fctx);
		avformat_free_context(fctx);
		av_frame_free(&bgRGB);
		return NULL;
	}
	JANUS_LOG(LOG_INFO, "Image loaded: %dx%d, %s\n",
		bgRGB->width, bgRGB->height, av_get_pix_fmt_name(bgRGB->format));

	avcodec_free_context(&ctx);
	avformat_close_input(&fctx);
	avformat_free_context(fctx);

	/* Done! */
	return bgRGB;
}

static void janus_ndi_avframe_free(AVFrame *frame) {
	if(frame)
		av_frame_free(&frame);
}

/* Helper to blit frames over a canvas */
static AVFrame *janus_ndi_blit_frameYUV(
		AVFrame *dst, AVFrame *src,
		int fromX, int fromY,
		int fromW, int fromH,
		int toX, int toY,
		enum AVPixelFormat pix_fmt) {
	if(!dst || !src)
		return NULL;

	/* Render */
	int X = 0, Y = 0;
	int maxX = toX+fromW, maxY = toY+fromH;
	if(pix_fmt == AV_PIX_FMT_YUV420P) {
		/* Simple copy */
		for(Y = toY; Y < maxY; Y++) {
			memcpy(dst->data[0] + Y*dst->linesize[0] + toX, src->data[0] + (Y-toY+fromY)*src->linesize[0] + fromX, fromW);
			memcpy(dst->data[1] + (Y/2)*dst->linesize[1] + toX/2, src->data[1] + ((Y-toY+fromY)/2)*src->linesize[1] + fromX/2, fromW/2);
			memcpy(dst->data[2] + (Y/2)*dst->linesize[2] + toX/2, src->data[2] + ((Y-toY+fromY)/2)*src->linesize[2] + fromX/2, fromW/2);
		}
	} else {
		/* Alpha channel involved, we need to blend */
		double alpha = 0, dy = 0, sy = 0, du = 0, su = 0, dv = 0, sv = 0;
		for(Y = toY; Y < maxY; Y++) {
			for(X = toX; X < maxX; X++) {
				alpha = (double)*(src->data[3] + (Y-toY+fromY)*src->linesize[3] + (X-toX+fromX))/255;
				if(alpha == 0.0) {
					/* Transparent, skip */
				} else if(alpha == 1.0) {
					/* Completely opaque, just copy */
					*(dst->data[0] + Y*dst->linesize[0] + X) = *(src->data[0] + (Y-toY+fromY)*src->linesize[0] + (X-toX+fromX));
					*(dst->data[1] + (Y/2)*dst->linesize[1] + X/2) = *(src->data[1] + ((Y-toY+fromY)/2)*src->linesize[1] + (X-toX+fromX)/2);
					*(dst->data[2] + (Y/2)*dst->linesize[2] + X/2) = *(src->data[2] + ((Y-toY+fromY)/2)*src->linesize[2] + (X-toX+fromX)/2);
				} else {
					/* Blend */
					dy = *(dst->data[0] + Y*dst->linesize[0] + X);
					sy = *(src->data[0] + (Y-toY+fromY)*src->linesize[0] + (X-toX+fromX));
					dy = (1.0-alpha)*dy + alpha*sy;
					*(dst->data[0] + Y*dst->linesize[0] + X) = (uint8_t)dy;
					du = *(dst->data[1] + (Y/2)*dst->linesize[1] + X/2);
					su = *(src->data[1] + ((Y-toY+fromY)/2)*src->linesize[1] + (X-toX+fromX)/2);
					du = (1.0-alpha)*du + alpha*su;
					*(dst->data[1] + (Y/2)*dst->linesize[1] + X/2) = (uint8_t)du;
					dv = *(dst->data[2] + (Y/2)*dst->linesize[2] + X/2);
					sv = *(src->data[2] + ((Y-toY+fromY)/2)*src->linesize[2] + (X-toX+fromX)/2);
					dv = (1.0-alpha)*dv + alpha*sv;
					*(dst->data[2] + (Y/2)*dst->linesize[2] + X/2) = (uint8_t)dv;
				}
			}
		}
	}

	/* Done */
	return dst;
}

/* Helper to generate an image from a url, taking into account resizing and/or aspect ratio */
static AVFrame *janus_ndi_generate_image(const char *path, int width, int height, gboolean keep_ratio,
		int r, int b, int g, int *error_code, char *error_cause, size_t error_cause_len) {
	AVFrame *image = NULL;
	if(path == NULL) {
		/* No placeholder provided, use the test pattern */
		image = test_pattern;
	} else {
		/* Retrieve the image */
		image = janus_ndi_download_image(path);
		if(image == NULL) {
			JANUS_LOG(LOG_ERR, "Error retrieving image\n");
			if(error_code)
				*error_code = JANUS_NDI_ERROR_IMAGE;
			if(error_cause && error_cause_len)
				g_snprintf(error_cause, error_cause_len, "Error retrieving image");
			return NULL;
		}
	}
	/* Image retrieved: scale it to what we need it to be */
	int t_width = (width == -1 ? image->width : width), sc_width = t_width;
	int t_height = (height == -1 ? image->height : height), sc_height = t_height;
	int sc_format = AV_PIX_FMT_UYVY422;
	/* Does the aspect ratio match? */
	float ar_source = (float)image->width/(float)image->height;
	float ar_target = (float)t_width/(float)t_height;
	if(keep_ratio && ar_source != ar_target) {
		/* Different aspect ratio */
		JANUS_LOG(LOG_INFO, "Aspect ratio is different: %.2f vs %.2f\n", ar_source, ar_target);
		if(ar_source < ar_target) {
			/* Let's keep the target height */
			sc_width = t_height*ar_source;
			if(sc_width % 2 != 0)
				sc_width--;
			sc_height = t_height;
		} else {
			/* Let's keep the target width */
			sc_width = t_width;
			sc_height = t_width/ar_source;
			if(sc_height % 2 != 0)
				sc_height--;
		}
		/* We need this format because we'll work on the image first */
		sc_format = AV_PIX_FMT_YUV420P;
	}
	/* Create the scaler */
	struct SwsContext *sws = sws_getContext(image->width, image->height, image->format,
		sc_width, sc_height, sc_format, SWS_BICUBIC, NULL, NULL, NULL);
	if(!sws) {
		JANUS_LOG(LOG_ERR, "Error creating scaler for image\n");
		if(error_code)
			*error_code = JANUS_NDI_ERROR_IMAGE;
		if(error_cause && error_cause_len)
			g_snprintf(error_cause, error_cause_len, "Error creating scaler for image");
		return NULL;
	}
	AVFrame *scaled_frame = av_frame_alloc();
	scaled_frame->width = sc_width;
	scaled_frame->height = sc_height;
	scaled_frame->format = sc_format;
	int err = av_image_alloc(scaled_frame->data, scaled_frame->linesize,
		scaled_frame->width, scaled_frame->height, scaled_frame->format, 1);
	if(err < 0) {
		av_frame_free(&scaled_frame);
		sws_freeContext(sws);
		JANUS_LOG(LOG_ERR, "Error allocating frame buffer: %d (%s)\n", err, av_err2str(err));
		if(error_code)
			*error_code = JANUS_NDI_ERROR_IMAGE;
		if(error_cause && error_cause_len)
			g_snprintf(error_cause, error_cause_len, "Error allocating frame buffer: %d (%s)", err, av_err2str(err));
		return NULL;
	}
	sws_scale(sws, (const uint8_t * const*)image->data, image->linesize,
		0, image->height, scaled_frame->data, scaled_frame->linesize);
	sws_freeContext(sws);
	/* If the aspect ratio didn't match, we're not done yet: we have a
	 * scaled image we now have to frame in the actual target resolution */
	if(sc_format == AV_PIX_FMT_YUV420P) {
		/* Create a frame of the target resolution */
		AVFrame *canvas = av_frame_alloc();
		canvas->width = t_width;
		canvas->height = t_height;
		canvas->format = AV_PIX_FMT_YUV420P;
		err = av_image_alloc(canvas->data, canvas->linesize,
			canvas->width, canvas->height, canvas->format, 1);
		if(err < 0) {
			av_frame_free(&canvas);
			av_free(scaled_frame->data[0]);
			av_frame_free(&scaled_frame);
			JANUS_LOG(LOG_ERR, "Error allocating canvas buffer: %d (%s)\n", err, av_err2str(err));
			if(error_code)
				*error_code = JANUS_NDI_ERROR_IMAGE;
			if(error_cause && error_cause_len)
				g_snprintf(error_cause, error_cause_len, "Error allocating canvas buffer: %d (%s)", err, av_err2str(err));
			return NULL;
		}
		/* Fill the canvas frame with the chosen color */
		int size = t_width * t_height;
		int y = ((66 * r + 129 * g + 25 * b + 128) >> 8) +  16;
		int u = (( -38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
		int v = (( 112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
		memset(canvas->data[0], y, size);
		memset(canvas->data[1], u, size/4);
		memset(canvas->data[2], v, size/4);
		/* Now blit the previously scaled frame on top of the canvas */
		int w = scaled_frame->width, h = scaled_frame->height;
		int tx = 0, ty = 0;
		if(w != canvas->width)
			tx += abs(w - canvas->width)/2;
		if(h != canvas->height)
			ty += abs(h - canvas->height)/2;
		tx = (tx < 0) ? 0 : tx;
		ty = (ty < 0) ? 0 : ty;
		tx = (tx > canvas->width - w) ? (canvas->width - w) : tx;
		ty = (ty > canvas->height - h) ? (canvas->height - h) : ty;
		janus_ndi_blit_frameYUV(canvas, scaled_frame, 0, 0, w, h, tx, ty, canvas->format);
		/* Now convert the final frame to the right format */
		av_free(scaled_frame->data[0]);
		av_frame_free(&scaled_frame);
		sws = sws_getContext(canvas->width, canvas->height, canvas->format,
			canvas->width, canvas->height, AV_PIX_FMT_UYVY422, SWS_BICUBIC, NULL, NULL, NULL);
		if(!sws) {
			av_free(canvas->data[0]);
			av_frame_free(&canvas);
			JANUS_LOG(LOG_ERR, "Error creating scaler for placeholder image\n");
			if(error_code)
				*error_code = JANUS_NDI_ERROR_IMAGE;
			if(error_cause && error_cause_len)
				g_snprintf(error_cause, error_cause_len, "Error creating scaler for placeholder image");
			return NULL;
		}
		scaled_frame = av_frame_alloc();
		scaled_frame->width = canvas->width;
		scaled_frame->height = canvas->height;
		scaled_frame->format = AV_PIX_FMT_UYVY422;
		err = av_image_alloc(scaled_frame->data, scaled_frame->linesize,
			scaled_frame->width, scaled_frame->height, AV_PIX_FMT_UYVY422, 1);
		if(err < 0) {
			av_free(canvas->data[0]);
			av_frame_free(&canvas);
			av_frame_free(&scaled_frame);
			sws_freeContext(sws);
			JANUS_LOG(LOG_ERR, "Error allocating frame buffer: %d (%s)\n", err, av_err2str(err));
			if(error_code)
				*error_code = JANUS_NDI_ERROR_IMAGE;
			if(error_cause && error_cause_len)
				g_snprintf(error_cause, error_cause_len, "Error allocating frame buffer: %d (%s)", err, av_err2str(err));
			return NULL;
		}
		sws_scale(sws, (const uint8_t * const*)canvas->data, canvas->linesize,
			0, canvas->height, scaled_frame->data, scaled_frame->linesize);
		sws_freeContext(sws);
		av_free(canvas->data[0]);
		av_frame_free(&canvas);
	}
	/* Done */
	return scaled_frame;
}

/* Helper to generate a 'disconnected' image of the right size */
static AVFrame *janus_ndi_generate_disconnected_image(const char *path,
		const char *color, int width, int height) {
	/* Parse the HTML color */
	int r = 0, g = 0, b = 0;
	sscanf(color, "%02x%02x%02x", &r, &g, &b);
	/* Generate the image */
	AVFrame *scaled_frame = janus_ndi_generate_image(path, width, height, TRUE,
		0, 0, 0, NULL, NULL, 0);
	if(scaled_frame == NULL)
		return NULL;
	/* Done */
	JANUS_LOG(LOG_INFO, "Created disconnected image: %dx%d, %s\n",
		scaled_frame->width, scaled_frame->height, av_get_pix_fmt_name(scaled_frame->format));
	return scaled_frame;
}

/* Helper to generate a placeholder image, taking into account resizing and/or aspect ratio */
static int janus_ndi_generate_placeholder_image(janus_ndi_sender *sender,
		const char *path, int width, int height, gboolean keep_ratio,
		int *error_code, char *error_cause, size_t error_cause_len) {
	AVFrame *scaled_frame = janus_ndi_generate_image(path, width, height, keep_ratio,
		0, 0, 0, error_code, error_cause, error_cause_len);
	if(scaled_frame == NULL)
		return -1;
	/* Done */
	janus_mutex_lock(&sender->mutex);
	if(sender->image != NULL) {
		av_free(sender->image->data[0]);
		av_frame_free(&sender->image);
	}
	sender->image = scaled_frame;
	janus_mutex_unlock(&sender->mutex);
	JANUS_LOG(LOG_INFO, "[%s] Created placeholder image: %dx%d, %s\n", sender->name,
		sender->image->width, sender->image->height, av_get_pix_fmt_name(sender->image->format));
	/* Finally, let's create a thread for this instance, if we don't have one yet */
	if(sender->thread != NULL)
		return 0;
	janus_refcount_increase(&sender->ref);
	GError *thread_error = NULL;
	char tname[16];
	g_snprintf(tname, sizeof(tname), "ndi %s", sender->name);
	sender->thread = g_thread_try_new(tname, &janus_ndi_placeholder_thread, sender, &thread_error);
	if(thread_error != NULL) {
		/* Error spawning thread */
		JANUS_LOG(LOG_ERR, "[%s] Got error %d (%s) trying to launch the thread...\n",
			sender->name, thread_error->code, thread_error->message ? thread_error->message : "??");
		g_error_free(thread_error);
		janus_refcount_decrease(&sender->ref);
		if(error_code)
			*error_code = JANUS_NDI_ERROR_THREAD;
		if(error_cause && error_cause_len)
			g_snprintf(error_cause, error_cause_len, "Error launching placeholder thread");
		return -1;
	}
	/* Done */
	return 0;
}
