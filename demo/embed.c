/* The demo's build tool: a file's bytes as a C array, compiled into the
 * demo (its music): embed <in> <out.c> <name>. */

#include <stdio.h>

int main(int argc, char** argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: embed <in> <out.c> <name>\n");
    return 1;
  }
  FILE* in = fopen(argv[1], "rb");
  FILE* out = fopen(argv[2], "w");
  if (!in || !out) {
    fprintf(stderr, "embed: can't open %s or %s\n", argv[1], argv[2]);
    return 1;
  }
  fprintf(out, "/* Generated from %s: don't edit. */\n", argv[1]);
  fprintf(out, "const unsigned char %s[] = {\n", argv[3]);
  unsigned long n = 0;
  int c;
  while ((c = fgetc(in)) != EOF) fprintf(out, "%d,%s", c, ++n % 32 ? "" : "\n");
  fprintf(out, "0};\nconst unsigned long %s_size = %luul;\n", argv[3], n);
  fclose(in);
  return fclose(out) ? 1 : 0;
}
