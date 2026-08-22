#pragma once

#include <gtk/gtk.h>

bool logipro_tray_start(GtkWindow* window, GApplication* application);
void logipro_tray_stop();
bool logipro_tray_available();
gboolean logipro_tray_close_request(GtkWindow* window, gpointer data);
