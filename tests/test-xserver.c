#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <linux/un.h> // FIXME: Should use sys version but UNIX_PATH_MAX not defined
#include <glib.h>

static gchar *socket_name;

#define BYTE_ORDER_MSB 'B'
#define BYTE_ORDER_LSB 'l'

#define PROTOCOL_MAJOR_VERSION 11
#define PROTOCOL_MINOR_VERSION 0

#define RELEASE_NUMBER 0
#define RESOURCE_ID_BASE 0x04e00000
#define RESOURCE_ID_MASK 0x001fffff
#define MOTION_BUFFER_SIZE 256
#define MAXIMUM_REQUEST_LENGTH 65535
#define BITMAP_FORMAT_SCANLINE_UNIT 32
#define BITMAP_FORMAT_SCANLINE_PAD 32
#define MIN_KEYCODE 8
#define MAX_KEYCODE 255
#define VENDOR "LightDM"

enum
{
    Success = 1
};

static size_t
pad (size_t length)
{
    if (length % 4 == 0)
        return 0;
    return 4 - length % 4;
}

static void
read_padding (size_t length, size_t *offset)
{
    *offset += length;
}

static guint8
read_card8 (guint8 *buffer, size_t buffer_length, size_t *offset)
{
    if (*offset >= buffer_length)
        return 0;
    (*offset)++;
    return buffer[*offset - 1];
}

static guint16
read_card16 (guint8 *buffer, size_t buffer_length, guint8 byte_order, size_t *offset)
{
    guint8 a, b;

    a = read_card8 (buffer, buffer_length, offset);
    b = read_card8 (buffer, buffer_length, offset);
    if (byte_order == BYTE_ORDER_MSB)
        return a << 8 | b;
    else
        return b << 8 | a;
}

static guint8 *
read_string8 (guint8 *buffer, size_t buffer_length, size_t string_length, size_t *offset)
{
    guint8 *string;
    int i;

    string = g_malloc (string_length + 1);
    for (i = 0; i < string_length; i++)
        string[i] = read_card8 (buffer, buffer_length, offset);
    string[i] = '\0';
    return string;
}

static void
write_card8 (guint8 *buffer, size_t buffer_length, guint8 value, size_t *offset)
{
    if (*offset >= buffer_length)
        return;
    buffer[*offset] = value;
    (*offset)++;
}

static void
write_padding (guint8 *buffer, size_t buffer_length, size_t length, size_t *offset)
{
    size_t i;
    for (i = 0; i < length; i++)
        write_card8 (buffer, buffer_length, 0, offset);
}

static void
write_card16 (guint8 *buffer, size_t buffer_length, guint8 byte_order, guint16 value, size_t *offset)
{
    if (byte_order == BYTE_ORDER_MSB)
    {
        write_card8 (buffer, buffer_length, value >> 8, offset);
        write_card8 (buffer, buffer_length, value & 0xFF, offset);
    }
    else
    {
        write_card8 (buffer, buffer_length, value & 0xFF, offset);
        write_card8 (buffer, buffer_length, value >> 8, offset);
    }
}

static void
write_card32 (guint8 *buffer, size_t buffer_length, guint8 byte_order, guint32 value, size_t *offset)
{
    if (byte_order == BYTE_ORDER_MSB)
    {
        write_card8 (buffer, buffer_length, value >> 24, offset);
        write_card8 (buffer, buffer_length, (value >> 16) & 0xFF, offset);
        write_card8 (buffer, buffer_length, (value >> 8) & 0xFF, offset);
        write_card8 (buffer, buffer_length, value & 0xFF, offset);
    }
    else
    {
        write_card8 (buffer, buffer_length, value & 0xFF, offset);
        write_card8 (buffer, buffer_length, (value >> 8) & 0xFF, offset);
        write_card8 (buffer, buffer_length, (value >> 16) & 0xFF, offset);
        write_card8 (buffer, buffer_length, value >> 24, offset);
    }
}

static void
write_string8 (guint8 *buffer, size_t buffer_length, const guint8 *value, size_t value_length, size_t *offset)
{
    size_t i;
    for (i = 0; i < value_length; i++)
        write_card8 (buffer, buffer_length, value[i], offset);
}

static void
decode_connect (guint8 *buffer, size_t buffer_length,
                guint8 *byte_order,
                guint16 *protocol_major_version, guint16 *protocol_minor_version,
                gchar **authorization_protocol_name,
                guint8 **authorization_protocol_data, guint16 *authorization_protocol_data_length)
{
    size_t offset = 0;
    guint16 n;

    *byte_order = read_card8 (buffer, buffer_length, &offset);
    read_padding (1, &offset);
    *protocol_major_version = read_card16 (buffer, buffer_length, *byte_order, &offset);
    *protocol_minor_version = read_card16 (buffer, buffer_length, *byte_order, &offset);
    n = read_card16 (buffer, buffer_length, *byte_order, &offset);
    *authorization_protocol_data_length = read_card16 (buffer, buffer_length, *byte_order, &offset);
    read_padding (2, &offset);
    *authorization_protocol_name = (gchar *) read_string8 (buffer, buffer_length, n, &offset);
    read_padding (pad (n), &offset);
    *authorization_protocol_data = read_string8 (buffer, buffer_length, *authorization_protocol_data_length, &offset);
    read_padding (pad (*authorization_protocol_data_length), &offset);
}

static size_t
encode_accept (guint8 *buffer, size_t buffer_length,
               guint8 byte_order)
{
    size_t offset = 0;
    guint8 additional_data_length;

    write_card8 (buffer, buffer_length, Success, &offset);
    write_padding (buffer, buffer_length, 1, &offset);
    write_card16 (buffer, buffer_length, byte_order, PROTOCOL_MAJOR_VERSION, &offset);
    write_card16 (buffer, buffer_length, byte_order, PROTOCOL_MINOR_VERSION, &offset);
    additional_data_length = 8 + (strlen (VENDOR) + pad (strlen (VENDOR))) / 4;
    write_card16 (buffer, buffer_length, byte_order, additional_data_length, &offset);

    /* Additional data */
    write_card32 (buffer, buffer_length, byte_order, RELEASE_NUMBER, &offset);
    write_card32 (buffer, buffer_length, byte_order, RESOURCE_ID_BASE, &offset);
    write_card32 (buffer, buffer_length, byte_order, RESOURCE_ID_MASK, &offset);
    write_card32 (buffer, buffer_length, byte_order, MOTION_BUFFER_SIZE, &offset);
    write_card16 (buffer, buffer_length, byte_order, strlen (VENDOR), &offset);
    write_card16 (buffer, buffer_length, byte_order, MAXIMUM_REQUEST_LENGTH, &offset);
    write_card8 (buffer, buffer_length, 0, &offset); // number of screens
    write_card8 (buffer, buffer_length, 0, &offset); // number of pixmap formats
    write_card8 (buffer, buffer_length, 0, &offset); // image-byte-order
    write_card8 (buffer, buffer_length, 0, &offset); // bitmap-format-bit-order
    write_card8 (buffer, buffer_length, BITMAP_FORMAT_SCANLINE_UNIT, &offset);
    write_card8 (buffer, buffer_length, BITMAP_FORMAT_SCANLINE_PAD, &offset);
    write_card8 (buffer, buffer_length, MIN_KEYCODE, &offset);
    write_card8 (buffer, buffer_length, MAX_KEYCODE, &offset);
    write_padding (buffer, buffer_length, 4, &offset);
    write_string8 (buffer, buffer_length, (guint8 *) VENDOR, strlen (VENDOR), &offset);
    write_padding (buffer, buffer_length, pad (strlen (VENDOR)), &offset);
    // pixmap formats
    // screens

    return offset;
}

static void
log_buffer (const gchar *text, const guint8 *buffer, size_t buffer_length)
{
    size_t i;

    printf ("%s", text);
    for (i = 0; i < buffer_length; i++)
        printf (" %02X", buffer[i]);
    printf ("\n");
}

static gboolean
socket_data_cb (GIOChannel *channel, GIOCondition condition, gpointer data)
{
    int s;
    guint8 buffer[MAXIMUM_REQUEST_LENGTH];
    ssize_t n_read;

    s = g_io_channel_unix_get_fd (channel);
    n_read = recv (s, buffer, MAXIMUM_REQUEST_LENGTH, 0);
    if (n_read < 0)
        g_warning ("Error reading from socket: %s", strerror (errno));
    else if (n_read == 0)
    {
        g_debug ("EOF");
        return FALSE;
    }
    else
    {
        guint8 byte_order;
        guint16 protocol_major_version, protocol_minor_version;
        gchar *authorization_protocol_name;
        guint8 *authorization_protocol_data;
        guint16 authorization_protocol_data_length;
        guint8 accept_buffer[MAXIMUM_REQUEST_LENGTH];
        size_t n_written;

        log_buffer ("Read", buffer, n_read);

        decode_connect (buffer, n_read,
                        &byte_order,
                        &protocol_major_version, &protocol_minor_version,
                        &authorization_protocol_name,
                        &authorization_protocol_data, &authorization_protocol_data_length);
        g_debug ("Got connect");

        n_written = encode_accept (accept_buffer, MAXIMUM_REQUEST_LENGTH, byte_order);
        g_debug ("Sending Success");
        send (s, accept_buffer, n_written, 0);
        log_buffer ("Wrote", accept_buffer, n_written);
    }

    return TRUE;
}

static gboolean
socket_connect_cb (GIOChannel *channel, GIOCondition condition, gpointer data)
{
    int s, data_socket;

    g_debug ("Connect");

    s = g_io_channel_unix_get_fd (channel);
    data_socket = accept (s, NULL, NULL);
    if (data_socket < 0)
        g_warning ("Error accepting connection: %s", strerror (errno));
    else
        g_io_add_watch (g_io_channel_unix_new (data_socket), G_IO_IN, socket_data_cb, NULL);

    return TRUE;
}

static void
quit ()
{
    unlink (socket_name);

    exit (EXIT_SUCCESS);
}

static void
quit_cb (int signum)
{
    quit ();
}

int
main (int argc, char **argv)
{
    int s;
    struct sockaddr_un address;
    GMainLoop *loop;

    signal (SIGINT, quit_cb);
    signal (SIGTERM, quit_cb);
 
    loop = g_main_loop_new (NULL, FALSE);

    s = socket (AF_UNIX, SOCK_STREAM, 0);
    if (s < 0)
    {
        g_warning ("Error opening socket: %s", strerror (errno));
        quit ();
    }

    socket_name = g_strdup_printf ("/tmp/.X11-unix/X%d", 1);

    address.sun_family = AF_UNIX;
    strncpy (address.sun_path, socket_name, UNIX_PATH_MAX);
    if (bind (s, (struct sockaddr *) &address, sizeof (address)) < 0)
    {
        g_warning ("Error binding socket: %s", strerror (errno));
        quit ();
    }

    if (listen (s, 10) < 0)
    {
        g_warning ("Error binding socket: %s", strerror (errno));
        quit ();
    }

    g_io_add_watch (g_io_channel_unix_new (s), G_IO_IN, socket_connect_cb, NULL);

    g_main_loop_run (loop);

    return EXIT_SUCCESS;
}