/* SPDX-License-Identifier: MIT
 * Included inside the PXADB service namespace after SendFrame is defined.
 * Hex chunks keep each frame bounded and never split UTF-8 code points. */
void SendInteractionInfo(unsigned long sequence) {
#if CONFIG_PXA_ENABLED
    char *document = static_cast<char *>(malloc(PXA_INTERACTION_MAX_BYTES + 1));
    if (!document) { SendFrame(sequence, "ERR", "interaction_memory"); return; }
    size_t size = 0;
    if (!pxa_host_interaction_describe(document, PXA_INTERACTION_MAX_BYTES + 1, &size)) {
        free(document);
        SendFrame(sequence, "OK", "schema=0;reason=interaction_unreported");
        return;
    }
    const char digits[] = "0123456789abcdef";
    for (size_t offset=0; offset<size; offset+=128) {
        const size_t count = size-offset < 128 ? size-offset : 128;
        char chunk[261] = "hex=";
        for (size_t i=0; i<count; ++i) {
            const auto byte = static_cast<unsigned char>(document[offset+i]);
            chunk[4+i*2]=digits[byte>>4]; chunk[5+i*2]=digits[byte&15];
        }
        chunk[4+count*2]=0;
        SendFrame(sequence, "DATA", chunk);
    }
    free(document);
    char result[64]; snprintf(result,sizeof(result),"schema=1;bytes=%u",static_cast<unsigned>(size));
    SendFrame(sequence,"OK",result);
#else
    SendFrame(sequence,"OK","schema=0;reason=interaction_unreported");
#endif
}
