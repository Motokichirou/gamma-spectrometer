# embed_fw.cmake — встраивает образ прошивки gamma_fw в пульт: генерирует fw_embed.h с массивом байт.
# Вызов: cmake -DFW_BIN=<путь к gamma_fw.bin> -DFW_TAG=<описание версии> -DOUT=<fw_embed.h> -P embed_fw.cmake
if(EXISTS "${FW_BIN}")
  file(READ "${FW_BIN}" HEX HEX)
  string(LENGTH "${HEX}" HEXLEN)
  math(EXPR NBYTES "${HEXLEN} / 2")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BODY "${HEX}")
  string(REGEX REPLACE "((0x..,){24})" "\\1\n" BODY "${BODY}")
  file(WRITE "${OUT}" "// СГЕНЕРИРОВАНО cmake/embed_fw.cmake из ${FW_BIN}\n#pragma once\n#define GAMMAPULT_FW_EMBEDDED 1\nstatic const char g_fw_tag[] = \"${FW_TAG}\";\nstatic const unsigned int g_fw_len = ${NBYTES};\nstatic const unsigned char g_fw_bin[] = {\n${BODY}\n};\n")
else()
  file(WRITE "${OUT}" "// СГЕНЕРИРОВАНО: образ прошивки не найден при сборке (${FW_BIN})\n#pragma once\nstatic const char g_fw_tag[] = \"\";\nstatic const unsigned int g_fw_len = 0;\nstatic const unsigned char g_fw_bin[1] = { 0 };\n")
endif()
