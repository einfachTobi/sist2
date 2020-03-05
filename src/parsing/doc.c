#include "doc.h"
#include "src/ctx.h"


#define STR_STARTS_WITH(x, y) (strncmp(y, x, sizeof(y) - 1) == 0)

__always_inline
static int should_read_part(const char *part) {

    LOG_DEBUGF("doc.c", "Got part : %s", part)

    if (part == NULL) {
        return FALSE;
    }

    if (    // Word
            STR_STARTS_WITH(part, "word/document.xml")
            || STR_STARTS_WITH(part, "word/footnotes.xml")
            || STR_STARTS_WITH(part, "word/endnotes.xml")
            || STR_STARTS_WITH(part, "word/footer")
            || STR_STARTS_WITH(part, "word/header")
            // PowerPoint
            || STR_STARTS_WITH(part, "ppt/slides/slide")
            || STR_STARTS_WITH(part, "ppt/notesSlides/slide")
            // Excel
            || STR_STARTS_WITH(part, "xl/worksheets/sheet")
            || STR_STARTS_WITH(part, "xl/sharedStrings.xml")
            || STR_STARTS_WITH(part, "xl/workbook.xml")
            ) {
        return TRUE;
    }

    return FALSE;
}

int extract_text(xmlDoc *xml, xmlNode *node, text_buffer_t *buf) {
    //TODO: Check which nodes are likely to have a 't' child, and ignore nodes that aren't
    xmlErrorPtr err = xmlGetLastError();
    if (err != NULL) {
        if (err->level == XML_ERR_FATAL) {
            LOG_ERRORF("doc.c", "Got fatal XML error while parsing document: %s", err->message)
            return -1;
        } else {
            LOG_ERRORF("doc.c", "Got recoverable XML error while parsing document: %s", err->message)
        }
    }

    for (xmlNode *child = node; child; child = child->next) {
        if (*child->name == 't' && *(child->name + 1) == '\0') {
            xmlChar *text = xmlNodeListGetString(xml, child->xmlChildrenNode, 1);

            if (text) {
                text_buffer_append_string0(buf, (char *) text);
                text_buffer_append_char(buf, ' ');
                xmlFree(text);
            }
        }

        extract_text(xml, child->children, buf);
    }
}

int xml_io_read(void *context, char *buffer, int len) {
    struct archive *a = context;
    return archive_read_data(a, buffer, len);
}

int xml_io_close(UNUSED(void *context)) {
    //noop
    return 0;
}

__always_inline
static int read_part(struct archive *a, text_buffer_t *buf, document_t *doc) {

    xmlDoc *xml = xmlReadIO(xml_io_read, xml_io_close, a, "/", NULL, XML_PARSE_RECOVER | XML_PARSE_NOWARNING | XML_PARSE_NOERROR | XML_PARSE_NONET);

    if (xml == NULL) {
        LOG_ERROR(doc->filepath, "Could not parse XML")
        return -1;
    }

    xmlNode *root = xmlDocGetRootElement(xml);
    if (root == NULL) {
        LOG_ERROR(doc->filepath, "Empty document")
        xmlFreeDoc(xml);
        return -1;
    }

    extract_text(xml, root, buf);
    xmlFreeDoc(xml);

    return 0;
}

void parse_doc(void *mem, size_t mem_len, document_t *doc) {

    if (mem == NULL) {
        return;
    }

    struct archive *a = archive_read_new();
    archive_read_support_format_zip(a);

    int ret = archive_read_open_memory(a, mem, mem_len);
    if (ret != ARCHIVE_OK) {
        LOG_ERRORF(doc->filepath, "Could not read archive: %s", archive_error_string(a))
        archive_read_free(a);
        return;
    }

    text_buffer_t buf = text_buffer_create(ScanCtx.content_size);

    struct archive_entry *entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        if (S_ISREG(archive_entry_stat(entry)->st_mode)) {
            const char *path = archive_entry_pathname(entry);

            if (should_read_part(path)) {
                ret = read_part(a, &buf, doc);
                if (ret != 0) {
                    break;
                }
            }
        }
    }

    if (buf.dyn_buffer.cur > 0) {
        text_buffer_terminate_string(&buf);

        meta_line_t *meta = malloc(sizeof(meta_line_t) + buf.dyn_buffer.cur);
        meta->key = MetaContent;
        strcpy(meta->strval, buf.dyn_buffer.buf);
        APPEND_META(doc, meta)
    }

    archive_read_close(a);
    archive_read_free(a);
    text_buffer_destroy(&buf);
}
