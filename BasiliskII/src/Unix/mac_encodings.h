typedef struct {
    const char *(*utf8_to_mac)(const char *, size_t);
    const char *(*mac_to_utf8)(const char *, size_t);
} EncodingFunctions;

EncodingFunctions get_encoding_functions();
