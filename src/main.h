#pragma once

constexpr const char *AU_VERSION = "0.1";

constexpr int AU_FORMAT_VERSION = 1; // TODO extract

int json2au(int argc, const char * const *argv);
int stats(int argc, const char * const *argv);
int grep(int argc, const char * const *argv);
int tail(int argc, const char * const *argv);
int cat(int argc, const char * const *argv);