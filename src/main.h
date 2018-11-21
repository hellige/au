#pragma once

constexpr const char *AU_VERSION = "0.2.2";

int json2au(int argc, const char * const *argv);
int stats(int argc, const char * const *argv);
int grep(int argc, const char * const *argv);
int zgrep(int argc, const char * const *argv);
int tail(int argc, const char * const *argv);
int cat(int argc, const char * const *argv);
int zindex(int argc, const char * const *argv);
