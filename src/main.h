#pragma once

constexpr const char *AU_VERSION = "0.1";

constexpr int AU_FORMAT_VERSION = 1; // TODO extract

int json2au(int argc, char **argv);
int stats(int argc, char **argv);
int grep(int argc, char **argv);
int tail(int argc, char **argv);