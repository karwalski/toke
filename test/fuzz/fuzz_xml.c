/*
 * fuzz_xml.c — libFuzzer entry point for the std.xml parser.
 *
 * Drives xml_parse() over arbitrary bytes and releases the resulting node
 * array with xml_free(). This exercises the flat/dotted-path element parser
 * used by the SOAP and XML stdlib surfaces (cross-ref 120.5 PAR-11 SOAP).
 *
 * Build:  make fuzz-xml   (requires clang with -fsanitize=fuzzer)
 * Story:  120.22
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/stdlib/xml.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 65536) return 0;

    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    XmlNodeArray arr = xml_parse(input);
    xml_free(&arr);

    free(input);
    return 0;
}
