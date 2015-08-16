#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <math.h>
#include <unistd.h>

#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gst/video/videooverlay.h>

/* We need access to the underlying Window IDs */
#include <gdk/gdk.h>
#if defined(GDK_WINDOWING_X11)
#include <gdk/gdkx.h>
#elif defined(GDK_WINDOWING_WIN32)
#include <gdk/gdkwin32.h>
#elif defined(GDK_WINDOWING_QUARTZ)
#include <gdk/gdkquartz.h>
#endif

#include <json-glib/json-glib.h>

#include <libsoup/soup.h>
#include <libsoup/soup-websocket.h>


#define PI 3.14159265358979323846


/* Structure to contain all our information, so we can pass it around */
typedef struct _CustomData {
	GstElement * pipeline;
	GstElement * playbin;

	GtkWidget * main_window;
	GtkWidget * video_widget;

	GList * websockets;

	gchar *  root_dir;
	GstState state;    /* Current state of the pipeline */
	gint64   duration; /* Duration of the clip, in nanoseconds */
} CustomData;


/* This function is called when the GUI toolkit creates the physical window that will hold the video.
 * At this point we can retrieve its handler(which has a different meaning depending on the windowing system)
 * and pass it to GStreamer through the XOverlay interface. */
static void realize_cb(GtkWidget * widget, CustomData * data) {
	GdkWindow *window = gtk_widget_get_window(widget);

	if(!gdk_window_ensure_native(window))
		g_error("Couldn't create native window needed for GstXOverlay!");

	/* Retrieve window handler from GDK */
	guintptr window_handle;
#if defined(GDK_WINDOWING_WIN32)
	window_handle = (guintptr)GDK_WINDOW_HWND(window);
#elif defined(GDK_WINDOWING_QUARTZ)
	window_handle = gdk_quartz_window_get_nsview(window);
#elif defined(GDK_WINDOWING_X11)
	window_handle = GDK_WINDOW_XID(window);
#endif
	/* Pass it to playbin, which implements XOverlay and will forward it to the video sink */
	gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(data->playbin), window_handle);
}


/* This function is called when the main window is closed */
static void delete_event_cb(GtkWidget * widget, GdkEvent * event, CustomData * data) {
	gst_element_set_state (data->pipeline, GST_STATE_READY);
	gtk_main_quit();
}


static void cairo_pattern_add_color_stop_hsva(cairo_pattern_t * pattern, double stop, double h, double s, double v, double a) {
	h = fmod(fmod(h, 360.0) + 360.0, 360.0);
    double c = v * s;
    double x = c * (1.0 - fabs(fmod(h / 60.0, 2) - 1.0));
    double m = v - c;
    double r, g, b;
         if(h >=   0.0 && h <  60.0) { r = c + m; g = x + m; b =     m; }
    else if(h >=  60.0 && h < 120.0) { r = x + m; g = c + m; b =     m; }
    else if(h >= 120.0 && h < 180.0) { r =     m; g = c + m; b = x + m; }
    else if(h >= 180.0 && h < 240.0) { r =     m; g = x + m; b = c + m; }
    else if(h >= 240.0 && h < 300.0) { r = x + m; g =     m; b = c + m; }
    else if(h >= 300.0 && h < 360.0) { r = c + m; g =     m; b = x + m; }
    else                             { r =     m; g =     m; b =     m; }
	cairo_pattern_add_color_stop_rgba(pattern, stop, r, g, b, a);
}


/* This function is called everytime the video window needs to be redrawn(due to damage/exposure,
 * rescaling, etc). GStreamer takes care of this in the PAUSED and PLAYING states, otherwise,
 * we simply draw a black rectangle to avoid garbage showing up. */
static gboolean draw_cb(GtkWidget * widget, cairo_t * cr, CustomData * data) {
	if(data->state < GST_STATE_PAUSED) {
		GtkAllocation allocation;

		/* Cairo is a 2D graphics library which we use here to clean the video window.
		 * It is used by GStreamer for other reasons, so it will always be available to us. */
		gtk_widget_get_allocation(widget, &allocation);

		cairo_surface_t * surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 32, 18);
		cairo_t *         context = cairo_create(surface);

		GDateTime * datetime  = g_date_time_new_now_local();
		double      seconds01 = g_date_time_get_seconds(datetime) / 60.0;

		cairo_pattern_t * gradient = cairo_pattern_create_linear(0.0, 0.0, cairo_image_surface_get_width(surface), cairo_image_surface_get_height(surface));
		cairo_pattern_add_color_stop_hsva(gradient, 0.0, seconds01 * 360.0        , 1.0, 0.4, 1.0);
		cairo_pattern_add_color_stop_hsva(gradient, 1.0, seconds01 * 360.0 + 120.0, 1.0, 0.4, 1.0);
		cairo_rectangle(context, 0.0, 0.0, cairo_image_surface_get_width(surface), cairo_image_surface_get_height(surface));
		cairo_set_source(context, gradient);
		cairo_fill(context);
		cairo_pattern_destroy(gradient);

		gradient = cairo_pattern_create_linear(0.0, 0.0, 0.0, cairo_image_surface_get_height(surface));
		cairo_pattern_add_color_stop_rgba(gradient, 0.0, 0.0, 0.0, 0.0, 0.8);
		cairo_pattern_add_color_stop_rgba(gradient, 0.3, 0.0, 0.0, 0.0, 0.0);
		cairo_pattern_add_color_stop_rgba(gradient, 0.7, 0.0, 0.0, 0.0, 0.0);
		cairo_pattern_add_color_stop_rgba(gradient, 1.0, 0.0, 0.0, 0.0, 0.8);
		cairo_rectangle(context, 0.0, 0.0, cairo_image_surface_get_width(surface), cairo_image_surface_get_height(surface));
		cairo_set_source(context, gradient);
		cairo_fill(context);
		cairo_pattern_destroy(gradient);

		int        noise_stride  = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, cairo_image_surface_get_width(surface));
		uint32_t * noise_data    = (uint32_t *)malloc(noise_stride * cairo_image_surface_get_height(surface));
		int        noise_seed    = 123456789;
		for(int y = 0; y < cairo_image_surface_get_height(surface); y++) {
			for(int x = 0; x < cairo_image_surface_get_width(surface); x++) {
				uint32_t * p  = ((uint32_t *)(((uint8_t *)noise_data) + noise_stride * y)) + x;
				double     a  = 1.0 + sin(noise_seed + (noise_seed % 10 + 10) * 0.1 * 2.0 * PI * seconds01) * 0.5;
				uint8_t    ca = (uint8_t)(a * 0.05 * 255.0);
				*p = (ca << 24) | (ca << 16) | (ca << 8) | (ca << 0); /* cairo expects premultiplied alpha */

				noise_seed = 1103515245 * noise_seed + 12345;
			}
		}
		cairo_surface_t * noise_surface = cairo_image_surface_create_for_data((unsigned char *)noise_data, CAIRO_FORMAT_ARGB32, cairo_image_surface_get_width(surface), cairo_image_surface_get_height(surface), noise_stride);
		cairo_set_source_surface(context, noise_surface, 0.0, 0.0);
		cairo_rectangle(context, 0.0, 0.0, cairo_image_surface_get_width(surface), cairo_image_surface_get_height(surface));
		cairo_fill(context);
		cairo_surface_destroy(noise_surface);
		free(noise_data);

		cairo_select_font_face(context, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
		cairo_font_options_t * options = cairo_font_options_create();
		cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_NONE);
		cairo_set_font_options(context, options);
		cairo_font_options_destroy(options);
		cairo_move_to(context, 2.0, 13.0);
		cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 0.4);
		gchar * time_str = g_date_time_format(datetime, "%H:%M");
		cairo_show_text(context, time_str);
		g_free(time_str);
		g_date_time_unref(datetime);

		cairo_save(cr);
		cairo_scale(cr, (double)allocation.width / cairo_image_surface_get_width(surface), (double)allocation.height / cairo_image_surface_get_height(surface));
		cairo_set_source_surface(cr, surface, 0.0, 0.0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
		cairo_rectangle(cr, 0, 0, cairo_image_surface_get_width(surface), cairo_image_surface_get_height(surface));
		cairo_fill(cr);
		cairo_restore(cr);

		cairo_destroy(context);
		cairo_surface_destroy(surface);

		surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width / 32, allocation.height / 18);
		context = cairo_create(surface);
		cairo_arc(context, 4.0, 4.0, 3.0, 1.0 * PI, 1.5 * PI);
		cairo_arc(context, cairo_image_surface_get_width(surface) - 4.0, 4.0, 3.0, 1.5 * PI, 2.0 * PI);
		cairo_arc(context, cairo_image_surface_get_width(surface) - 4.0, cairo_image_surface_get_height(surface) - 4.0, 3.0, 2.0 * PI, 0.5 * PI);
		cairo_arc(context, 4.0, cairo_image_surface_get_height(surface) - 4.0, 3.0, 0.5 * PI, 1.0 * PI);
		cairo_line_to(context, 1.0, 4.0);
		cairo_line_to(context, 0.0, 4.0);
		cairo_line_to(context, 0.0, cairo_image_surface_get_height(surface));
		cairo_line_to(context, cairo_image_surface_get_width(surface), cairo_image_surface_get_height(surface));
		cairo_line_to(context, cairo_image_surface_get_width(surface), 0.0);
		cairo_line_to(context, 0.0, 0.0);
		cairo_line_to(context, 0.0, 4.0);
		cairo_set_source_rgb(context, 0.0, 0.0, 0.0);
		cairo_fill(context);

		cairo_set_source_surface(cr, surface, 0.0, 0.0);
		cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
		cairo_rectangle(cr, 0, 0, allocation.width, allocation.height);
		cairo_fill(cr);

		cairo_destroy(context);
		cairo_surface_destroy(surface);
	}

	return FALSE;
}


/* This function is called periodically to refresh the GUI */
static gboolean refresh_ui(CustomData *data) {
	/* We do not want to update anything unless we are in the PAUSED or PLAYING states */
	if(data->state < GST_STATE_PAUSED) {
		gtk_widget_queue_draw(data->video_widget);
		return TRUE;
	}

	/* If we didn't know it yet, query the stream duration */
	if(!GST_CLOCK_TIME_IS_VALID(data->duration)) {
		if(!gst_element_query_duration(data->playbin, GST_FORMAT_TIME, &data->duration))
			g_printerr("Could not query current duration.\n");
	}
	return TRUE;
}


/* This function is called when an error message is posted on the bus */
static void error_cb(GstBus *bus, GstMessage *msg, CustomData *data) {
	/* Print error details on the screen */
	GError *err;
	gchar  *debug_info;
	gst_message_parse_error(msg, &err, &debug_info);
	g_printerr("Error received from element %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
	g_printerr("Debugging information: %s\n", debug_info ? debug_info : "none");
	g_clear_error(&err);
	g_free(debug_info);

	/* Set the pipeline to READY(which stops playback) */
	gst_element_set_state(data->pipeline, GST_STATE_READY);
}


/* This function is called when an End-Of-Stream message is posted on the bus.
 * We just set the pipeline to READY(which stops playback) */
static void eos_cb(GstBus *bus, GstMessage *msg, CustomData *data) {
	g_print("End-Of-Stream reached.\n");
	gst_element_set_state(data->pipeline, GST_STATE_READY);
}


/* This function is called when the pipeline changes states. We use it to keep track of the current state. */
static void state_changed_cb(GstBus *bus, GstMessage *msg, CustomData *data) {
	GstState old_state, new_state, pending_state;
	gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
	if(GST_MESSAGE_SRC(msg) == GST_OBJECT(data->pipeline)) {
		data->state = new_state;
		if(old_state == GST_STATE_READY && new_state == GST_STATE_PAUSED) {
			/* For extra responsiveness, we refresh the GUI as soon as we reach the PAUSED state */
			refresh_ui(data);
		}
	}
}


static void server_callback(SoupServer * server, SoupMessage * msg, const char * path, GHashTable * query, SoupClientContext * context, CustomData * data) {
	g_print("[%s] %s\n", msg->method, path);

	if(msg->method != SOUP_METHOD_GET && msg->method != SOUP_METHOD_HEAD) {
		soup_message_set_status(msg, SOUP_STATUS_NOT_IMPLEMENTED);
		return;
	}

	char * file_path = g_strdup_printf("public%s", path); /* path always starts with a '/' */

	struct stat st;
	if(stat(file_path, &st) == -1) {
		if(errno == EPERM)
			soup_message_set_status(msg, SOUP_STATUS_FORBIDDEN);
		else if(errno == ENOENT)
			soup_message_set_status(msg, SOUP_STATUS_NOT_FOUND);
		else
			soup_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR);
		g_free(file_path);
		return;
	}

	if(!S_ISREG(st.st_mode)) {
		soup_message_set_status(msg, SOUP_STATUS_FORBIDDEN);
		g_free(file_path);
		return;
	}

	if(msg->method == SOUP_METHOD_GET) {
		GMappedFile * mapping = g_mapped_file_new(file_path, FALSE, NULL);
		if(!mapping) {
			soup_message_set_status(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR);
			g_free(file_path);
			return;
		}

		SoupBuffer * buffer = soup_buffer_new_with_owner(g_mapped_file_get_contents(mapping), g_mapped_file_get_length(mapping), mapping, (GDestroyNotify)g_mapped_file_unref);
		soup_message_body_append_buffer(msg->response_body, buffer);
		soup_buffer_free(buffer);
	}

	soup_message_set_status(msg, SOUP_STATUS_OK);
	g_free(file_path);
}


static void soup_websocket_connection_send_json(SoupWebsocketConnection * connection, JsonBuilder * builder) {
	JsonGenerator * generator = json_generator_new();

	JsonNode * node = json_builder_get_root(builder);
	json_generator_set_root(generator, node);

	gchar * json = json_generator_to_data(generator, NULL);
	soup_websocket_connection_send_text(connection, json);

	json_node_free(node);
	g_object_unref(generator);
	g_free(json);
}


static void send_directory_listing(SoupWebsocketConnection * connection, const char * path) {
	GList * directories = NULL,
	      * files       = NULL;

	if(path == NULL || strlen(path) == 0 || path[strlen(path) - 1] != '/') {
		g_printerr("[ERR] bad path for directory listing: %s\n", path);
		return;
	}

	/* Collect all directories and files from `path' */
	GError * error;
	GDir * dir = g_dir_open(path, 0, &error);
	if(dir == NULL) {
		g_printerr("[ERR] cannot open directory `%s'\n", path);
		return;
	}

	const gchar * entry;
	while((entry = g_dir_read_name(dir)) != NULL) {
		if(entry[0] == '.')
			continue;

		char * file_path = g_strdup_printf("%s%s", path, entry);

		struct stat st;
		if(stat(file_path, &st) == 0) {
			if(S_ISDIR(st.st_mode)) {
				char * name = g_strdup(entry);
				directories = g_list_append(directories, name);
			}
			else if(S_ISREG(st.st_mode)) {
				char * name = g_strdup(entry);
				files = g_list_append(files, name);
			}
		}

		g_free(file_path);
	}

	g_dir_close(dir);

	/* Sort the resulting lists */
	directories = g_list_sort(directories, (GCompareFunc)g_ascii_strcasecmp);
	files       = g_list_sort(files      , (GCompareFunc)g_ascii_strcasecmp);

	/* Build and send the resulting Json */
	JsonBuilder * builder = json_builder_new();
	json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "type");
		json_builder_add_string_value(builder, "browse");

		json_builder_set_member_name(builder, "path");
		json_builder_add_string_value(builder, path);

		json_builder_set_member_name(builder, "directories");
		json_builder_begin_array(builder);
		for(GList * l = directories; l != NULL; l = l->next)
			json_builder_add_string_value(builder, l->data);
		json_builder_end_array(builder);

		json_builder_set_member_name(builder, "files");
		json_builder_begin_array(builder);
		for(GList * l = files; l != NULL; l = l->next) {
			char * file_path = g_strdup_printf("%s%s", path, (const char *)l->data);
			struct stat st;
			stat(file_path, &st);

			json_builder_begin_object(builder);
				json_builder_set_member_name(builder, "name");
				json_builder_add_string_value(builder, l->data);

				json_builder_set_member_name(builder, "size");
				json_builder_add_int_value(builder, st.st_size);
			json_builder_end_object(builder);

			g_free(file_path);
		}
		json_builder_end_array(builder);
	json_builder_end_object(builder);
	soup_websocket_connection_send_json(connection, builder);
	g_object_unref(builder);

	g_list_free_full(directories, g_free);
	g_list_free_full(files, g_free);
}


static void send_status(SoupWebsocketConnection * connection, CustomData * data) {
	JsonBuilder * builder = json_builder_new();
	json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "type");
		json_builder_add_string_value(builder, "status");

		json_builder_set_member_name(builder, "length");
		json_builder_add_double_value(builder, data->duration / 1000000000.0);

		json_builder_set_member_name(builder, "state");
		GstState state = GST_STATE_NULL;
		gst_element_get_state(data->pipeline, &state, NULL, GST_CLOCK_TIME_NONE);
		switch(state) {
			case GST_STATE_PAUSED:  json_builder_add_string_value(builder, "paused" ); break;
			case GST_STATE_PLAYING: json_builder_add_string_value(builder, "playing"); break;
			default:                json_builder_add_string_value(builder, "stopped"); break;
		}

		gint64 position = 0;
		gst_element_query_position(data->playbin, GST_FORMAT_TIME, &position);
		json_builder_set_member_name(builder, "position");
		json_builder_add_double_value(builder, position / 1000000000.0);

		gchar * filename;
		g_object_get(GST_OBJECT(data->playbin), "current-uri", &filename, NULL);
		json_builder_set_member_name(builder, "filename");
		json_builder_add_string_value(builder, filename);
	json_builder_end_object(builder);
	soup_websocket_connection_send_json(connection, builder);
	g_object_unref(builder);
}


static void broadcast_status(CustomData * data) {
	for(GList * l = data->websockets; l != NULL; l = l->next)
		send_status(l->data, data);
}


static void websocket_onclosed(SoupWebsocketConnection * self, CustomData * data) {
	printf("[WS] closed\n");
	data->websockets = g_list_remove(data->websockets, g_object_ref(self));
	g_object_unref(self);
}


static void websocket_onerror(SoupWebsocketConnection * self, GError * error, CustomData * data) {
	printf("[WS] error\n");
}


static void websocket_onmessage(SoupWebsocketConnection * self, gint type, GBytes * message, CustomData * data) {
	if(type == SOUP_WEBSOCKET_DATA_TEXT) {
		gsize size;
		char * json = g_bytes_unref_to_data(message, &size);

		JsonParser * parser = json_parser_new();
		if(!json_parser_load_from_data(parser, json, size, NULL)) {
			g_printerr("[ERR] bad json request: %s\n", json);
			g_free(json);
			return;
		}
		g_free(json);

		JsonNode * node = json_parser_get_root(parser);
		if(json_node_get_node_type(node) != JSON_NODE_OBJECT) {
			g_printerr("[ERR] bad json request: %s\n", json);
			return;
		}

		JsonObject * object = json_node_get_object(node);

		const gchar * type = json_object_get_string_member(object, "type");
		if(g_strcmp0(type, "browse") == 0) {
			const gchar * path = json_object_get_string_member(object, "path");
			if(path != NULL) {
				g_print("[WS] browse %s\n", path);
				send_directory_listing(self, path);
			}
		}
		else if(g_strcmp0(type, "load") == 0) {
			const gchar * path = json_object_get_string_member(object, "path");
			if(path != NULL) {
				g_print("[WS] load %s\n", path);
				char * str = g_strdup_printf("file://%s", path);
				gst_element_set_state(data->pipeline, GST_STATE_READY);
				g_object_set(data->playbin, "uri", str, NULL);
				gst_element_set_state(data->pipeline, GST_STATE_PLAYING);
				g_free(str);
				broadcast_status(data);
			}
		}
		else if(g_strcmp0(type, "play") == 0) {
			gst_element_set_state(data->pipeline, GST_STATE_PLAYING);
			g_print("[WS] play\n");
			broadcast_status(data);
		}
		else if(g_strcmp0(type, "pause") == 0) {
			gst_element_set_state(data->pipeline, GST_STATE_PAUSED);
			g_print("[WS] pause\n");
			broadcast_status(data);
		}
		else if(g_strcmp0(type, "stop") == 0) {
			gst_element_set_state(data->pipeline, GST_STATE_READY);
			g_print("[WS] stop\n");
			broadcast_status(data);
		}
		else if(g_strcmp0(type, "fullscreen") == 0) {
			gtk_window_fullscreen(GTK_WINDOW(data->main_window));
			g_print("[WS] fullscreen\n");
			broadcast_status(data);
		}
		else if(g_strcmp0(type, "seek") == 0) {
			double percent  = json_object_get_double_member(object, "percent");
			gint64 position = percent / 100.0 * data->duration;
			g_print("[WS] seek %f%%\n", percent);
			gst_element_seek_simple(data->pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, position);
			broadcast_status(data);
		}
		else if(g_strcmp0(type, "jump") == 0) {
			gint64 ms = json_object_get_int_member(object, "ms");
			g_print("[WS] jump %" G_GINT64_FORMAT "ms\n", ms);
			gint64 position;
			if(gst_element_query_position(data->playbin, GST_FORMAT_TIME, &position)) {
				position += ms * GST_MSECOND;
				gst_element_seek_simple(data->pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, position);
				broadcast_status(data);
			}
		}

		g_object_unref(parser);
	}
}


static void websocket_onconnect(SoupServer * server, SoupWebsocketConnection * connection, const char * path, SoupClientContext * client, CustomData * data) {
	printf("[WS] connect\n");
	data->websockets = g_list_append(data->websockets, g_object_ref(connection)); /* important: keep a reference */
	g_signal_connect(connection, "closed",  (GCallback)websocket_onclosed , data);
	g_signal_connect(connection, "error",   (GCallback)websocket_onerror  , data);
	g_signal_connect(connection, "message", (GCallback)websocket_onmessage, data);

	send_status(connection, data);
	send_directory_listing(connection, data->root_dir);
}


int main(int argc, char * argv[]) {
	/* Initialize GTK */
	gtk_init(&argc, &argv);

	/* Initialize GStreamer */
	gst_init(&argc, &argv);

	/* Initialize our data structure */
	CustomData data;
	memset(&data, 0, sizeof(data));
	data.duration = GST_CLOCK_TIME_NONE;
	if(argc == 2)
		data.root_dir = g_strdup(argv[1]);
	else
		data.root_dir = g_get_current_dir();
	if(data.root_dir[strlen(data.root_dir) - 1] != '/') {
		gchar * str = g_strdup_printf("%s/", data.root_dir);
		g_free(data.root_dir);
		data.root_dir = str;
	}

	/* Create the elements */
	data.pipeline = gst_pipeline_new(NULL);
	data.playbin = gst_element_factory_make("playbin", "playbin");
	if(!data.pipeline || !data.playbin) {
		g_printerr("Not all elements could be created.\n");
		return -1;
	}
	gst_bin_add(GST_BIN(data.pipeline), data.playbin);

	/* Create the GUI */
	data.main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	g_signal_connect(G_OBJECT(data.main_window), "delete-event", G_CALLBACK(delete_event_cb), (gpointer)&data);

	data.video_widget = gtk_drawing_area_new();
	g_signal_connect(data.video_widget, "realize", G_CALLBACK(realize_cb), (gpointer)&data);
	g_signal_connect(data.video_widget, "draw"   , G_CALLBACK(draw_cb   ), (gpointer)&data);

	gtk_container_add(GTK_CONTAINER(data.main_window), data.video_widget);
	gtk_window_set_title(GTK_WINDOW(data.main_window), "Banana Player");
	gtk_window_set_default_size(GTK_WINDOW(data.main_window), 1280, 720);

	gtk_widget_show_all(data.main_window);
	gtk_window_fullscreen(GTK_WINDOW(data.main_window));
	gtk_window_set_keep_above(GTK_WINDOW(data.main_window), 1);

	GdkCursor * cursor = gdk_cursor_new_for_display(gdk_display_get_default(), GDK_BLANK_CURSOR);
	gdk_window_set_cursor(gtk_widget_get_window(GTK_WIDGET(data.main_window)), cursor);

	/* Instruct the bus to emit signals for each received message, and connect to the interesting signals */
	GstBus * bus = gst_element_get_bus(data.pipeline);
	gst_bus_add_signal_watch(bus);
	g_signal_connect(G_OBJECT(bus), "message::error"        , (GCallback)error_cb        , &data);
	g_signal_connect(G_OBJECT(bus), "message::eos"          , (GCallback)eos_cb          , &data);
	g_signal_connect(G_OBJECT(bus), "message::state-changed", (GCallback)state_changed_cb, &data);
	gst_object_unref(bus);

	/* Register a function that GLib will call every second */
	g_timeout_add(120, (GSourceFunc)refresh_ui, &data);

	SoupServer * server = soup_server_new(SOUP_SERVER_SERVER_HEADER, "banana-player", NULL);
	GError *error = NULL;
	soup_server_listen_all(server, 3001, 0, &error);
	soup_server_add_handler(server, "/", (SoupServerCallback)server_callback, &data, NULL);
	soup_server_add_websocket_handler(server, "/ws", NULL, NULL, (SoupServerWebsocketCallback)websocket_onconnect, &data, NULL);

	/* Start the GTK main loop. We will not regain control until gtk_main_quit is called. */
	gtk_main();

	/* Free resources */
	gst_element_set_state(data.pipeline, GST_STATE_NULL);
	gst_object_unref(data.pipeline);
	gst_object_unref(data.playbin);
	g_free(data.root_dir);
	return 0;
}
