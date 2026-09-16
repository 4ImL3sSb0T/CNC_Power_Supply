#include "common_def.h"

const char *error_code_name(exit_code_t code) {
    switch (code)
    {
    EXIT_CODE_NAME_CASE(EXIT_OK);
    EXIT_CODE_NAME_CASE(EXIT_SKIP);
    EXIT_CODE_NAME_CASE(EXIT_IN_PROGRESS);
    EXIT_CODE_NAME_CASE(EXIT_FAIL);
    EXIT_CODE_NAME_CASE(EXIT_TIMEOUT);
    EXIT_CODE_NAME_CASE(EXIT_INVALID_PARAM);
    EXIT_CODE_NAME_CASE(EXIT_NOT_SUPPORTED);
    EXIT_CODE_NAME_CASE(EXIT_NO_MEMORY);
    EXIT_CODE_NAME_CASE(EXIT_BUSY);
    EXIT_CODE_NAME_CASE(EXIT_NO_RESOURCE);
    EXIT_CODE_NAME_CASE(EXIT_ALREADY_EXISTS);
    EXIT_CODE_NAME_CASE(EXIT_DOES_NOT_EXIST);
    EXIT_CODE_NAME_CASE(EXIT_NOT_INITIALIZED);
    EXIT_CODE_NAME_CASE(EXIT_ALREADY_INITIALIZED);
    EXIT_CODE_NAME_CASE(EXIT_CRC_MISMATCH);
    EXIT_CODE_NAME_CASE(EXIT_HW_FAILURE);
    EXIT_CODE_NAME_CASE(EXIT_UNKNOWN);
    default:
        return "UNKNOWN_EXIT_CODE";
    }
}

void assert_fun(exit_code_t code) {
    const char *name = error_code_name(code);
    switch (code)
    {
    case EXIT_OK:
        break;
    case EXIT_SKIP:
        zf_log(1, (char *)name);
        break;
    case EXIT_IN_PROGRESS:
        zf_log(1, (char *)name);
        break;
    default:
        zf_log(0, (char *)name);
        break;
    }
}

