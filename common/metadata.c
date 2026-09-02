#include "metadata.h"

#include <stdlib.h>
#include <string.h>

void avmetadata_reset(struct avmetadata_s *list)
{
	if (list->count == 0)
		return;

	for (int i = 0; i < list->count; i++) {
		avmetadata_item_free(list->array[i]);
		list->array[i] = NULL;
	}

	list->count = 0;
}

void avmetadata_init(struct avmetadata_s *list)
{
	memset(list, 0, sizeof(*list));
	list->count = 0; /* Redundant */
}

void avmetadata_clone(struct avmetadata_s *dst, struct avmetadata_s *src)
{
	if (src->count == 0)
		return;

	avmetadata_reset(dst);

	for (int i = 0; i < src->count; i++) {
		struct avmetadata_item_s *item = avmetadata_item_clone(src->array[i]);
		if (!item)
			continue;
		dst->array[dst->count++] = item;
	}
}

int avmetadata_count_by_type(struct avmetadata_s *src, enum avmetadata_item_type_e item_type)
{
	int c = 0;

	for (int i = 0; i < src->count; i++) {
		if (src->array[i]->item_type == item_type)
			c++;
	}

	return c;
}

struct avmetadata_item_s *avmetadata_item_alloc(int lengthBytes, enum avmetadata_item_type_e item_type)
{
	struct avmetadata_item_s *dst = malloc(lengthBytes);
	if (!dst) {
		printf("%s(%d)\n", __func__, lengthBytes);
		return NULL;
	}

	switch (item_type) {
	case AVMETADATA_SECTION_SCTE35:
	default:
		dst->dataLengthAlloc = lengthBytes;
		dst->dataLengthBytes = 0;
		dst->item_type = item_type;
		dst->data = malloc(dst->dataLengthAlloc);
		if (!dst->data) {
			printf("%s(%d) data alloc %d bytes\n", __func__, lengthBytes, dst->dataLengthAlloc);
			free(dst);
			dst = NULL;
		}
	}

	return dst;
}

struct avmetadata_item_s *avmetadata_item_clone(struct avmetadata_item_s *src)
{
	struct avmetadata_item_s *dst = avmetadata_item_alloc(src->dataLengthAlloc, src->item_type);
	if (!dst)
		return NULL;

	switch (src->item_type) {
	case AVMETADATA_SECTION_SCTE35:
	default:
		dst->outputStreamId = src->outputStreamId;
		dst->lineNr = src->lineNr;
		dst->dataLengthBytes = src->dataLengthBytes;
		memcpy(dst->data, src->data, src->dataLengthAlloc);
	}

	return dst;
}

void avmetadata_item_free(struct avmetadata_item_s *src)
{
	if (!src)
		return;

	switch (src->item_type) {
	case AVMETADATA_SECTION_SCTE35:
	default:
		if (src->data) {
			free(src->data);
			src->data = NULL;
		}
		free(src);
	}
}

const char *avmetadata_item_name(struct avmetadata_item_s *src)
{
	switch (src->item_type) {
	case AVMETADATA_SECTION_SCTE35:
		return "SECTION_SCTE35";
	default:
		return "UNDEFINED";
	}
}

int avmetadata_item_data_write(struct avmetadata_item_s *dst, const unsigned char *buf, int lengthBytes)
{
	if (dst->dataLengthAlloc < lengthBytes)
		return -1;

	memcpy(dst->data, buf, lengthBytes);
	dst->dataLengthBytes = lengthBytes;

	return 0;
}

void avmetadata_item_dprintf(int fd, struct avmetadata_item_s *src)
{
	switch (src->item_type) {
	case AVMETADATA_SECTION_SCTE35:
	default:
		dprintf(fd, "item %p '%s' dataLengthBytes %d (0x%x) data %p -- \n",
			src,
			avmetadata_item_name(src),
			src->dataLengthBytes,
			src->dataLengthBytes,
			src->data);
		for (int i = 0; i < (src->dataLengthBytes > 16 ? 16 : src->dataLengthBytes); i++)
			dprintf(fd, "%02x ", src->data[i]);
		dprintf(fd, "\n");
	}
}

int avmetadata_item_data_realloc(struct avmetadata_item_s *src, int lengthBytes)
{
	src->data = realloc(src->data, lengthBytes);
	if (!src->data)
		return -1;

	src->dataLengthAlloc = lengthBytes;

	return 0;
}

int avmetadata_item_data_set_linenr(struct avmetadata_item_s *src, int lineNr)
{
	src->lineNr = lineNr;
	return 0;
}

int avmetadata_item_data_get_linenr(struct avmetadata_item_s *src)
{
	return src->lineNr;
}

int avmetadata_item_data_set_outputstreamid(struct avmetadata_item_s *src, int outputStreamId)
{
	src->outputStreamId = outputStreamId;
	return 0;
}

int avmetadata_item_data_get_outputstreamid(struct avmetadata_item_s *src)
{
	return src->outputStreamId;
}

