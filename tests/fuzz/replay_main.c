// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file replay_main.c
 * @brief Runs each file named on the command line through LLVMFuzzerTestOneInput once: the seed
 * corpora and regression inputs as an ordinary test, without libFuzzer.
 */

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS // fopen
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size);

int main(int argc, char** argv)
{
  static uint8_t input[1 << 20];
  for (int i = 1; i < argc; i++)
  {
    FILE* const file = fopen(argv[i], "rb");
    if (file == NULL)
    {
      fprintf(stderr, "cannot open %s\n", argv[i]);
      return 1;
    }
    size_t const size = fread(input, 1, sizeof(input), file);
    int const more = fgetc(file);
    fclose(file);
    if (more != EOF)
    {
      fprintf(stderr, "%s exceeds %zu bytes\n", argv[i], sizeof(input));
      return 1;
    }
    (void)LLVMFuzzerTestOneInput(input, size);
  }
  printf("replayed %d inputs\n", argc - 1);
  return 0;
}
