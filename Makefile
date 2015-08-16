all:
	gcc -Wall -g -std=c99 src/main.c -o banana-player `pkg-config --cflags --libs gstreamer-video-1.0 gtk+-3.0 gstreamer-1.0 libsoup-2.4 json-glib-1.0` -lm
