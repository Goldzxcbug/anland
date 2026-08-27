/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#pragma once

#include <gio/gio.h>

typedef struct _MetaAnlandClipboard MetaAnlandClipboard;
typedef struct _MetaAnlandTransport MetaAnlandTransport;
typedef struct _MetaBackend MetaBackend;

MetaAnlandClipboard * meta_anland_clipboard_new (MetaBackend *backend);
void meta_anland_clipboard_free (MetaAnlandClipboard *clipboard);

void meta_anland_clipboard_set_transport (MetaAnlandClipboard *clipboard,
                                          MetaAnlandTransport *transport);

gboolean meta_anland_clipboard_set_from_consumer (MetaAnlandClipboard *clipboard,
                                                   GBytes              *contents);
