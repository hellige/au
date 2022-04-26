#pragma once

namespace au {

int json2au(int argc, const char * const *argv);
int stats(int argc, const char * const *argv);
int grep(int argc, const char * const *argv);
int zgrep(int argc, const char * const *argv);
int tail(int argc, const char * const *argv);
int ztail(int argc, const char * const *argv);
int cat(int argc, const char * const *argv);
int zcat(int argc, const char * const *argv);
int zindex(int argc, const char * const *argv);

}
