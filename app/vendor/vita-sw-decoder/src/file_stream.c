#include "vita_sw_decoder.h"

#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>

typedef struct {
	SceUID fd;
} VitaSwDecoderFileHandle;

static int file_read(void *opaque, void *buffer, size_t size) {
	VitaSwDecoderFileHandle *handle = (VitaSwDecoderFileHandle *)opaque;
	return handle ? sceIoRead(handle->fd, buffer, size) : -1;
}

static int64_t file_seek(void *opaque, int64_t offset, int whence) {
	VitaSwDecoderFileHandle *handle = (VitaSwDecoderFileHandle *)opaque;
	return handle ? sceIoLseek(handle->fd, offset, whence) : -1;
}

static void file_close(void *opaque) {
	VitaSwDecoderFileHandle *handle = (VitaSwDecoderFileHandle *)opaque;
	if (!handle) return;
	if (handle->fd >= 0) sceIoClose(handle->fd);
	free(handle);
}

static int file_open(void *opaque, VitaSwDecoderStreamHandle *out) {
	const char *path = (const char *)opaque;
	if (!path || !out) return -1;
	VitaSwDecoderFileHandle *handle = calloc(1, sizeof(*handle));
	if (!handle) return -1;
	handle->fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (handle->fd < 0) {
		int ret = handle->fd;
		free(handle);
		return ret;
	}
	SceOff size = sceIoLseek(handle->fd, 0, SCE_SEEK_END);
	sceIoLseek(handle->fd, 0, SCE_SEEK_SET);
	memset(out, 0, sizeof(*out));
	out->opaque = handle;
	out->read = file_read;
	out->seek = file_seek;
	out->close = file_close;
	out->size = size >= 0 ? size : -1;
	return 0;
}

void vita_sw_decoder_file_stream_factory(const char *path,
	                             VitaSwDecoderStreamFactory *factory) {
	if (!factory) return;
	memset(factory, 0, sizeof(*factory));
	factory->opaque = (void *)path;
	factory->open = file_open;
}
