#define GST_USE_UNSTABLE_API

#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <iostream>
#include <getopt.h>

// ===================== Config =====================
struct Config {
    gchar *codec;
    gint bitrate;
    gint fps;
    gint width;
    gint height;
    gchar *device;
};

// ===================== Globals =====================
static struct Config config;
static GstElement *pipeline = NULL;
static GstElement *webrtc = NULL;
static GMainLoop *loop = NULL;
static SoupWebsocketConnection *ws_conn = NULL;
static gchar *peer_id = NULL;
static gchar *my_id = NULL;
static gboolean offer_in_progress = FALSE;

static const gchar *server_url = "ws://192.168.25.69:8080";

// ===================== Decls =====================
static void on_offer_created(GstPromise *promise, gpointer user_data);
static void force_renegotiate();
static void on_negotiation_needed(GstElement *element, gpointer user_data);
static void on_ice_candidate(GstElement *webrtc, guint mlineindex, gchar *candidate, gpointer user_data);
static void send_ice_candidate_message(guint mlineindex, const gchar *candidate);
static void on_incoming_stream(GstElement *webrtc, GstPad *pad, gpointer user_data);
static gboolean on_bus_message(GstBus *bus, GstMessage *message, gpointer user_data);
static void reset_peer_state();
static std::string build_pipeline_string();
static gboolean build_and_start_pipeline();     // NEW
static void stop_and_destroy_pipeline();        // NEW
static gboolean restart_pipeline();             // NEW

// ===================== Utils: signaling =====================
static void send_json_message(JsonObject *msg) {
    if (!ws_conn) { g_printerr("WebSocket not connected\n"); return; }
    JsonNode *root = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(root, msg);
    gchar *text = json_to_string(root, FALSE);
    g_print("[ws->] %s\n", text);
    soup_websocket_connection_send_text(ws_conn, text);
    g_free(text);
    json_node_free(root);
}

static void reset_peer_state() {
    g_print("Resetting peer state\n");
    if (peer_id) { g_free(peer_id); peer_id = NULL; }
    offer_in_progress = FALSE;
}

// ===================== Pipeline build/start/stop =====================
static std::string build_pipeline_string() {
    const char *encoder, *parser, *payloader, *encoding_name;
    int payload = 96;

    if (g_strcmp0(config.codec, "h265") == 0) {
        encoder = "omxh265enc";
        parser  = "h265parse";
        payloader = "rtph265pay";
        encoding_name = "H265";
    } else {
        encoder = "omxh264enc";
        parser  = "h264parse";
        payloader = "rtph264pay";
        encoding_name = "H264";
    }

    char pipeline_buf[4096];
    snprintf(pipeline_buf, sizeof(pipeline_buf),
        "webrtcbin name=webrtcbin bundle-policy=max-bundle latency=100 "
        "stun-server=stun://stun.l.google.com:19302 "
        "v4l2src device=%s ! "
        "video/x-raw,width=%d,height=%d,framerate=%d/1 ! "
        "videoconvert ! "
        "queue max-size-buffers=3 leaky=downstream ! "
        "%s target-bitrate=%d control-rate=2 ! "
        "%s ! "
        "%s config-interval=1 pt=%d ! "
        "application/x-rtp,media=video,encoding-name=%s,payload=%d ! "
        "webrtcbin. "
        "audiotestsrc is-live=true wave=silence ! "
        "audioconvert ! audioresample ! queue ! "
        "opusenc ! rtpopuspay pt=97 ! "
        "application/x-rtp,media=audio,encoding-name=OPUS,payload=97 ! "
        "webrtcbin.",
        config.device, config.width, config.height, config.fps,
        encoder, config.bitrate * 1000,
        parser,
        payloader, payload, encoding_name, payload
    );

    g_print("\n=== Configuration ===\n");
    g_print("Codec:      %s\n", config.codec);
    g_print("Resolution: %dx%d\n", config.width, config.height);
    g_print("Framerate:  %d fps\n", config.fps);
    g_print("Bitrate:    %d kbps\n", config.bitrate);
    g_print("Device:     %s\n", config.device);
    g_print("====================\n\n");

    return std::string(pipeline_buf);
}

static void connect_webrtc_signals() {
    g_assert(webrtc != NULL);
    g_signal_connect(webrtc, "on-negotiation-needed",  G_CALLBACK(on_negotiation_needed), NULL);
    g_signal_connect(webrtc, "on-ice-candidate",       G_CALLBACK(on_ice_candidate), NULL);
    g_signal_connect(webrtc, "pad-added",              G_CALLBACK(on_incoming_stream), NULL);
    g_signal_connect(webrtc, "notify::ice-gathering-state",
                     G_CALLBACK(+[](GstElement* w, GParamSpec*, gpointer){
                         GstWebRTCICEGatheringState s; g_object_get(w,"ice-gathering-state",&s,nullptr);
                         const char* str = (s==GST_WEBRTC_ICE_GATHERING_STATE_NEW)?"new":
                                           (s==GST_WEBRTC_ICE_GATHERING_STATE_GATHERING)?"gathering":
                                           (s==GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE)?"complete":"unknown";
                         g_print("ICE gathering state: %s\n", str);
                     }), NULL);
    g_signal_connect(webrtc, "notify::ice-connection-state",
                     G_CALLBACK(+[](GstElement* w, GParamSpec*, gpointer){
                         GstWebRTCICEConnectionState s; g_object_get(w,"ice-connection-state",&s,nullptr);
                         const char* str = (s==GST_WEBRTC_ICE_CONNECTION_STATE_NEW)?"new":
                                           (s==GST_WEBRTC_ICE_CONNECTION_STATE_CHECKING)?"checking":
                                           (s==GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED)?"connected":
                                           (s==GST_WEBRTC_ICE_CONNECTION_STATE_COMPLETED)?"completed":
                                           (s==GST_WEBRTC_ICE_CONNECTION_STATE_FAILED)?"failed":
                                           (s==GST_WEBRTC_ICE_CONNECTION_STATE_DISCONNECTED)?"disconnected":
                                           (s==GST_WEBRTC_ICE_CONNECTION_STATE_CLOSED)?"closed":"unknown";
                         g_print("ICE connection state: %s\n", str);
                         if (s==GST_WEBRTC_ICE_CONNECTION_STATE_FAILED ||
                             s==GST_WEBRTC_ICE_CONNECTION_STATE_DISCONNECTED ||
                             s==GST_WEBRTC_ICE_CONNECTION_STATE_CLOSED) {
                             reset_peer_state();
                         }
                     }), NULL);
}

static gboolean build_and_start_pipeline() {
    GError *error = NULL;
    std::string s = build_pipeline_string();

    pipeline = gst_parse_launch(s.c_str(), &error);
    if (error) {
        g_printerr("Failed to create pipeline: %s\n", error->message);
        g_error_free(error);
        pipeline = NULL;
        return FALSE;
    }

    webrtc = gst_bin_get_by_name(GST_BIN(pipeline), "webrtcbin");
    if (!webrtc) {
        g_printerr("webrtcbin not found in pipeline\n");
        gst_object_unref(pipeline); pipeline = NULL;
        return FALSE;
    }

    connect_webrtc_signals();

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_watch(bus, on_bus_message, NULL);
    gst_object_unref(bus);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_print("Pipeline started\n");
    return TRUE;
}

static void stop_and_destroy_pipeline() {
    if (!pipeline) return;
    g_print("Stopping pipeline...\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    if (webrtc) { gst_object_unref(webrtc); webrtc = NULL; }
    gst_object_unref(pipeline); pipeline = NULL;
    g_print("Pipeline destroyed\n");
}

static gboolean restart_pipeline() {
    stop_and_destroy_pipeline();
    return build_and_start_pipeline();
}

// ===================== Signaling handlers =====================
static void on_message(SoupWebsocketConnection *conn, SoupWebsocketDataType type,
                       GBytes *message, gpointer user_data) {
    if (type != SOUP_WEBSOCKET_DATA_TEXT) return;

    gsize size;
    const gchar *data = (const gchar *)g_bytes_get_data(message, &size);
    gchar *text = g_strndup(data, size);
    g_print("[ws<-] %s\n", text);

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, text, -1, NULL)) {
        g_printerr("Failed to parse JSON\n");
        g_free(text); g_object_unref(parser); return;
    }
    JsonObject *object = json_node_get_object(json_parser_get_root(parser));
    const gchar *msg_type = json_object_get_string_member(object, "type");

    if (g_strcmp0(msg_type, "registered") == 0) {
        my_id = g_strdup(json_object_get_string_member(object, "id"));
        g_print("Registered with ID: %s\n", my_id);

    } else if (g_strcmp0(msg_type, "answer") == 0) {
        const gchar *sdp_text = json_object_get_string_member(object, "sdp");
        const gchar *from_id  = json_object_get_string_member(object, "from");
        if (!peer_id || g_strcmp0(peer_id, from_id) != 0) { g_free(peer_id); peer_id = g_strdup(from_id); }

        GstSDPMessage *sdp; gst_sdp_message_new(&sdp);
        gst_sdp_message_parse_buffer((guint8 *)sdp_text, strlen(sdp_text), sdp);
        auto *answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
        GstPromise *promise = gst_promise_new();
        g_signal_emit_by_name(webrtc, "set-remote-description", answer, promise);
        gst_promise_interrupt(promise); gst_promise_unref(promise);
        gst_webrtc_session_description_free(answer);
        offer_in_progress = FALSE;

    } else if (g_strcmp0(msg_type, "ice-candidate") == 0) {
        if (!json_object_has_member(object, "candidate")) goto out;
        JsonObject *cand = json_object_get_object_member(object, "candidate");
        const gchar *candidate_str = json_object_get_string_member(cand, "candidate");
        if (!candidate_str || !*candidate_str) goto out;
        guint sdp_mline_index = json_object_get_int_member(cand, "sdpMLineIndex");
        g_print("✓ Adding ICE candidate [%u]: %s\n", sdp_mline_index, candidate_str);
        g_signal_emit_by_name(webrtc, "add-ice-candidate", sdp_mline_index, candidate_str);

    } else if (g_strcmp0(msg_type, "request-offer") == 0) {
        const gchar *from_id = json_object_has_member(object,"from") ? json_object_get_string_member(object,"from") : NULL;
        if (from_id) g_print("Received request-offer from %s\n", from_id);

        // IMPORTANT: start a fresh pipeline for a new viewer
        if (!restart_pipeline()) {
            g_printerr("Failed to restart pipeline\n"); goto out;
        }
        reset_peer_state();                  // clear previous peer id / flags
        force_renegotiate();                 // create a clean new offer

    } else if (g_strcmp0(msg_type, "peer-left") == 0) {
        const gchar *left_id = json_object_has_member(object,"id") ? json_object_get_string_member(object,"id") : NULL;
        g_print("Peer left notification: %s\n", left_id ? left_id : "(unknown)");
        if (left_id && peer_id && g_strcmp0(left_id, peer_id) == 0) {
            g_print("Our peer disconnected; restarting pipeline\n");
            reset_peer_state();
            restart_pipeline();              // ensure clean state for the next viewer
        }
    }

out:
    g_free(text);
    g_object_unref(parser);
}

// ===================== ICE / offer =====================
static void send_ice_candidate_message(guint mlineindex, const gchar *candidate) {
    JsonObject *ice = json_object_new();
    json_object_set_string_member(ice, "candidate", candidate);
    json_object_set_int_member(ice, "sdpMLineIndex", mlineindex);
    // NOTE: do NOT set sdpMid (mids change across renegotiations)

    JsonObject *msg = json_object_new();
    json_object_set_string_member(msg, "type", "ice-candidate");
    json_object_set_object_member(msg, "candidate", ice);
    if (peer_id) json_object_set_string_member(msg, "to", peer_id);
    send_json_message(msg);
    json_object_unref(msg);
}

static void on_ice_candidate(GstElement * /*webrtc*/, guint mlineindex,
                             gchar *candidate, gpointer /*user_data*/) {
    g_print("Generated ICE candidate: %s\n", candidate);
    send_ice_candidate_message(mlineindex, candidate);
}

static void on_negotiation_needed(GstElement * /*element*/, gpointer /*user_data*/) {
    g_print("Negotiation needed signal received\n");
    // We only create an offer when the viewer asks (request-offer),
    // because we rebuild the pipeline on every session anyway.
}

static void on_incoming_stream(GstElement * /*webrtc*/, GstPad * /*pad*/, gpointer /*user_data*/) {
    g_print("Received incoming stream (unexpected for sender)\n");
}

static void force_renegotiate() {
    if (!webrtc) { g_printerr("Cannot renegotiate: webrtc not available\n"); return; }
    if (offer_in_progress) { g_print("Offer already in progress, skipping\n"); return; }
    g_print("Creating new offer for reconnection\n");
    offer_in_progress = TRUE;
    GstPromise *promise = gst_promise_new_with_change_func(on_offer_created, NULL, NULL);
    g_signal_emit_by_name(webrtc, "create-offer", NULL, promise);
}

static void on_offer_created(GstPromise *promise, gpointer /*user_data*/) {
    GstWebRTCSessionDescription *offer = NULL;
    const GstStructure *reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref(promise);
    if (!offer) { g_printerr("Failed to create offer\n"); offer_in_progress = FALSE; return; }

    g_print("Offer created, setting local description\n");
    GstPromise *p = gst_promise_new();
    g_signal_emit_by_name(webrtc, "set-local-description", offer, p);
    gst_promise_interrupt(p); gst_promise_unref(p);

    gchar *sdp_text = gst_sdp_message_as_text(offer->sdp);
    JsonObject *msg = json_object_new();
    json_object_set_string_member(msg, "type", "offer");
    json_object_set_string_member(msg, "sdp", sdp_text);
    send_json_message(msg);
    g_free(sdp_text);
    json_object_unref(msg);
    gst_webrtc_session_description_free(offer);
}

// ===================== Bus =====================
static gboolean on_bus_message(GstBus * /*bus*/, GstMessage *message, gpointer /*user_data*/) {
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError *err; gchar *dbg;
            gst_message_parse_error(message, &err, &dbg);
            g_printerr("Error: %s\n", err->message);
            g_printerr("Debug: %s\n", dbg);
            g_error_free(err); g_free(dbg);
            g_main_loop_quit(loop);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError *err; gchar *dbg;
            gst_message_parse_warning(message, &err, &dbg);
            g_printerr("Warning: %s\n", err->message);
            g_error_free(err); g_free(dbg);
            break;
        }
        case GST_MESSAGE_EOS:
            g_print("End of stream\n");
            g_main_loop_quit(loop);
            break;
        default: break;
    }
    return TRUE;
}

// ===================== WS connect =====================
static void on_websocket_connected(GObject *session, GAsyncResult *res, gpointer user_data) {
    GError *error = NULL;
    ws_conn = soup_session_websocket_connect_finish(SOUP_SESSION(session), res, &error);
    if (error) {
        g_printerr("WebSocket connection failed: %s\n", error->message);
        g_error_free(error);
        g_main_loop_quit(loop);
        return;
    }
    g_print("✓ WebSocket connected to signaling server\n");

    g_signal_connect(ws_conn, "message", G_CALLBACK(on_message), NULL);
    g_signal_connect(ws_conn, "closed",
        G_CALLBACK(+[](SoupWebsocketConnection*, gpointer data){
            g_print("WebSocket closed\n");
            g_main_loop_quit((GMainLoop*)data);
        }), loop);

    // (optional metadata)
    JsonObject* join = json_object_new();
    json_object_set_string_member(join, "type", "join");
    json_object_set_string_member(join, "room", "default");
    json_object_set_string_member(join, "clientType", "sender");
    send_json_message(join);
    json_object_unref(join);
}

// ===================== Args / main =====================
static void print_usage(const char *prog) {
    g_print("Usage: %s [OPTIONS]\n\n", prog);
    g_print("  --codec=CODEC       h264 or h265 (default: h264)\n");
    g_print("  --bitrate=KBPS      bitrate kbps (default: 2000)\n");
    g_print("  --fps=FPS           framerate (default: 30)\n");
    g_print("  --width=WIDTH       width (default: 1280)\n");
    g_print("  --height=HEIGHT     height (default: 720)\n");
    g_print("  --device=PATH       camera device (default: /dev/video0)\n");
    g_print("  --help              show this help\n");
}

static gboolean parse_arguments(int argc, char *argv[]) {
    config.codec = g_strdup("h264");
    config.bitrate = 2000;
    config.fps = 30;
    config.width = 1280;
    config.height = 720;
    config.device = g_strdup("/dev/video0");

    struct option long_options[] = {
        {"codec",  required_argument, 0, 'c'},
        {"bitrate",required_argument, 0, 'b'},
        {"fps",    required_argument, 0, 'f'},
        {"width",  required_argument, 0, 'w'},
        {"height", required_argument, 0, 'H'},
        {"device", required_argument, 0, 'd'},
        {"help",   no_argument,       0, '?'},
        {0,0,0,0}
    };
    int c, idx=0;
    while ((c = getopt_long(argc, argv, "c:b:f:w:H:d:?", long_options, &idx)) != -1) {
        switch (c) {
            case 'c':
                g_free(config.codec); config.codec = g_strdup(optarg);
                if (g_strcmp0(config.codec,"h264")!=0 && g_strcmp0(config.codec,"h265")!=0) {
                    g_printerr("Error: codec must be h264 or h265\n"); return FALSE;
                }
                break;
            case 'b': config.bitrate = atoi(optarg); if (config.bitrate<=0) { g_printerr("bitrate>0\n"); return FALSE; } break;
            case 'f': config.fps = atoi(optarg); if (config.fps<=0||config.fps>120) { g_printerr("fps 1..120\n"); return FALSE; } break;
            case 'w': config.width = atoi(optarg); if (config.width<=0){ g_printerr("width>0\n"); return FALSE; } break;
            case 'H': config.height= atoi(optarg); if (config.height<=0){ g_printerr("height>0\n"); return FALSE; } break;
            case 'd': g_free(config.device); config.device = g_strdup(optarg); break;
            case '?': default: print_usage(argv[0]); return FALSE;
        }
    }
    return TRUE;
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);
    if (!parse_arguments(argc, argv)) return -1;

    loop = g_main_loop_new(NULL, FALSE);

    if (!build_and_start_pipeline()) return -1;

    // Connect to signaling
    SoupSession *session = soup_session_new();
    SoupMessage *msg = soup_message_new("GET", server_url);
    g_print("Connecting to signaling server: %s\n", server_url);
    soup_session_websocket_connect_async(session, msg, NULL, NULL, NULL, on_websocket_connected, NULL);

    g_main_loop_run(loop);

    // Cleanup
    stop_and_destroy_pipeline();
    if (ws_conn) { soup_websocket_connection_close(ws_conn, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL); g_object_unref(ws_conn); }
    g_object_unref(session);
    g_main_loop_unref(loop);

    g_free(my_id); g_free(peer_id);
    g_free(config.codec); g_free(config.device);
    return 0;
}
