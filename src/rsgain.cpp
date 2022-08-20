/*
 * Loudness normalizer based on the EBU R128 standard
 *
 * Copyright (c) 2014, Alessandro Ghedini
 * All rights reserved.
 *
 * rsgain by complexlogic, 2022
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <getopt.h>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
}
#include <ebur128.h>
#include <fmt/core.h>

#include "rsgain.hpp"
#include "tag.hpp"
#include "config.h"
#include "scan.hpp"
#include "output.hpp"
#include "easymode.hpp"

#ifdef _WIN32
#include <windows.h>
void init_console(void);
void set_cursor_visibility(BOOL setting, BOOL *previous);
#endif

static void help_main(void);
static void version(void);
static inline void help_custom(void);
static inline void help_easy(void);

// Global variables
int multithread = 0;
ProgressBar progress_bar;

#ifdef _WIN32
HANDLE console;
BOOL initial_cursor_visibility;
#endif

#ifdef _WIN32
 static void init_console()
{
    SetConsoleCP(CP_UTF8);
    console = CreateFileA(
        "CONOUT$",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        0
    );
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleMode(console, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    set_cursor_visibility(FALSE, &initial_cursor_visibility);
}

// Make the console cursor invisible for the progress bar
static void set_cursor_visibility(BOOL setting, BOOL *previous)
{
    CONSOLE_CURSOR_INFO info;
    GetConsoleCursorInfo(console, &info);
    if (previous != NULL) {
        *previous = info.bVisible;
    }
    info.bVisible = setting;
    SetConsoleCursorInfo(console, &info);
}
#endif

void quit(int status)
{
    #ifdef _WIN32
    if (initial_cursor_visibility) {
        set_cursor_visibility(TRUE, NULL);
    }
    #endif
    exit(status);
}

void parse_target_loudness(const char *value, Config &config)
{
    int loudness = atoi(value);
    if (loudness < MIN_TARGET_LOUDNESS || loudness > MAX_TARGET_LOUDNESS) {
        output_error("Invalid target loudness value '{}'", value);
        return;
    }
    config.target_loudness = loudness;        
}

void parse_tag_mode(const char *value, Config &config)
{
    std::string_view valid_modes = "dis";
    size_t pos = valid_modes.find_first_of(*value);
    if (pos != std::string::npos) {
        config.tag_mode = valid_modes[pos];
    }
    else {
        output_error("Invalid tag mode: '{}'", value);
    }        
}

bool parse_clip_mode(const char *value, char &mode)
{
    static const std::string_view valid_modes = "npa";
    size_t pos = valid_modes.find_first_of(*value);
    if (pos != std::string::npos) {
        mode = valid_modes[pos];
        return true;
    }
    output_error("Invalid clip mode: '{}'", value);
    return false;
}

void parse_id3v2_version(const char *value, Config &config)
{
    config.id3v2version = atoi(value);
    if (!(config.id3v2version == 3) && !(config.id3v2version == 4))
        output_error("Invalid ID3v2 version '{}'; only 3 and 4 are supported.", value);
}

void parse_max_peak_level(const char *value, Config &config)
{
    char *rest = NULL;
    float max_peak = strtod(value, &rest);
    if (rest == value || !isfinite(max_peak))
        output_error("Invalid max peak level '{}'", value);

    config.max_peak_level = max_peak;
}

// Parse Easy Mode command line arguments
static void easy_mode(int argc, char *argv[])
{
    int rc, i;
    char *overrides_file = NULL;
    const char *short_opts = "+hqm:o:";
    static struct option long_opts[] = {
        { "help",         no_argument,       NULL, 'h' },
        { "quiet",        no_argument,       NULL, 'q' },

        { "multithread",  required_argument, NULL, 'm' },
        { "override",     required_argument, NULL, 'o' },
        { 0, 0, 0, 0 }
    };
    while ((rc = getopt_long(argc, argv, short_opts, long_opts, &i)) != -1) {
        switch (rc) {
            case 'h':
                help_easy();
                quit(EXIT_SUCCESS);
                break;

            case 'q':
                quiet = true;
                break;
            
            case 'm':
                multithread = atoi(optarg);
                if (multithread < 2)
                    multithread = 0;
                break;
            
            case 'o':
                overrides_file = optarg;
                break;
        }
    }

    if (argc == optind) {
        fmt::print("Error: You must specific the directory to scan\n");
        quit(EXIT_FAILURE);
    }

    scan_easy(argv[optind], overrides_file);
}

// Parse Custom Mode command line arguments
static void custom_mode(int argc, char *argv[])
{
    int rc, i;
    unsigned nb_files   = 0;

    const char *short_opts = "+ac:K:tl:Oqs:LSI:h?";
    static struct option long_opts[] = {
        { "album",         no_argument,       NULL, 'a' },

        { "clip-mode",       no_argument,     NULL, 'c' },
        { "max-peak",      required_argument, NULL, 'K' },
        { "true-peak",     required_argument, NULL, 't' },

        { "loudness",      required_argument, NULL, 'l' },

        { "output",        no_argument,       NULL, 'O' },
        { "quiet",         no_argument,       NULL, 'q' },

        { "tagmode",       required_argument, NULL, 's' },
        { "lowercase",     no_argument,       NULL, 'L' },
        { "strip-tags",    no_argument,       NULL, 'S' },
        { "id3v2-version", required_argument, NULL, 'I' },

        { "help",         no_argument,        NULL, 'h' },
        { 0, 0, 0, 0 }
    };

    Config config = {
        .tag_mode = 's',
        .target_loudness = RG_TARGET_LOUDNESS,
        .max_peak_level = EBU_R128_MAX_PEAK,
        .true_peak = false,
        .clip_mode = 'n',
        .do_album = false,
        .tab_output = false,
        .lowercase = false,
        .strip = false,
        .id3v2version = 4
    };

    while ((rc = getopt_long(argc, argv, short_opts, long_opts, &i)) !=-1) {
        switch (rc) {
            case 'a':
                config.do_album = true;
                break;

            case 'c':
                parse_clip_mode(optarg, config.clip_mode);
                break;

            case 'K': {
                parse_max_peak_level(optarg, config);
                break;
            }

            case 't': {
                config.true_peak = true;
                break;
            }

            case 'l': {
                parse_target_loudness(optarg, config);
                break;
            }

            case 'O':
                config.tab_output = true;
                break;

            case 'q':
                quiet = 1;
                break;

            case 's': {
                parse_tag_mode(optarg, config);
                break;
            }

            case 'L':
                config.lowercase = true;
                break;

            case 'S':
                config.strip = true;
                break;

            case 'I':
                parse_id3v2_version(optarg, config);
                break;

            case '?':
                if (optopt == 0) {
                    // actual option '-?'
                    help_custom();
                    quit(EXIT_SUCCESS);
                } else {
                    // getopt error, message already printed
                    quit(EXIT_FAILURE);	// error
                }
            case 'h':
                help_custom();
                quit(EXIT_SUCCESS);
                break;
        }
    }
    
    nb_files = argc - optind;
    if (!nb_files) {
        output_fail("No files were specified\n");
        quit(EXIT_FAILURE);
    }

    ScanJob job;
    if (job.add_files(argv + optind, nb_files)) {
        output_fail("No valid files were specified");
        quit(EXIT_FAILURE);
    }
    job.scan(config);
}

// Parse main arguments
int main(int argc, char *argv[]) {
    int rc, i;
    char *command = NULL;
    
    const char *short_opts = "+hv";
    static struct option long_opts[] = {
        { "help",         no_argument,       NULL, 'h' },
        { "version",      no_argument,       NULL, 'v' },
        { 0, 0, 0, 0 }
    };
  
#ifdef _WIN32
    init_console();
#endif 

    while ((rc = getopt_long(argc, argv, short_opts, long_opts, &i)) !=-1) {
        switch (rc) {
            case 'h':
                help_main();
                quit(EXIT_SUCCESS);
                break;

            case 'v':
                version();
                quit(EXIT_SUCCESS);
                break;
        }
    }

    if (argc == optind) {
        help_main();
        quit(EXIT_SUCCESS);
    }
    
    // Parse and run command
    command = argv[optind];
    char **subargs = argv + optind;
    int num_subargs = argc - optind;
    optind = 1;
    if (!strcmp(command, "easy")) {
        easy_mode(num_subargs, subargs);
    }
    else if (!strcmp(command, "custom")) {
        custom_mode(num_subargs, subargs);
    }
    else {
        output_fail("Unrecognized command '{}'\n", command);
        quit(EXIT_FAILURE);
    }
    quit(EXIT_SUCCESS);
}


static void help_main(void) {
    fmt::print(COLOR_RED "Usage: " COLOR_OFF "{}{}{} [OPTIONS] <command> ...\n", COLOR_GREEN, EXECUTABLE_TITLE, COLOR_OFF);

    fmt::print("{} {} supports writing tags to the following file types:\n", PROJECT_NAME, PROJECT_VERSION);
    fmt::print("  FLAC (.flac), Ogg (.ogg, .oga, .spx), OPUS (.opus), MP2 (.mp2),\n");
    fmt::print("  MP3 (.mp3), MP4 (.m4a), WMA (.wma), WavPack (.wv), APE (.ape).\n");
    fmt::print("  Experimental: WAV (.wav), AIFF (.aiff, .aif, .snd).\n");

    fmt::print("\n");
    fmt::print(COLOR_RED "Options:\n" COLOR_OFF);

    CMD_HELP("--help",     "-h", "Show this help");
    CMD_HELP("--version",  "-v", "Show version number");
    
    fmt::print("\n");
    fmt::print(COLOR_RED "Commands:\n" COLOR_OFF);

    CMD_CMD("easy",     "Easy Mode:   Recursively scan a directory with recommended settings");
    CMD_CMD("custom",   "Custom Mode: Scan individual files with custom settings");
    fmt::print("\n");
    fmt::print("Run '{} easy --help' or '{} custom --help' for more information.", EXECUTABLE_TITLE, EXECUTABLE_TITLE);

    fmt::print("\n\n");
    fmt::print("Please report any issues to " PROJECT_URL "/issues\n\n");
}


static inline void help_easy(void) {
    fmt::print(COLOR_RED "Usage: " COLOR_OFF "{}{}{} easy [OPTIONS] DIRECTORY\n", COLOR_GREEN, EXECUTABLE_TITLE, COLOR_OFF);

    fmt::print("  Easy Mode recursively scans a directory using the recommended settings for each\n");
    fmt::print("  file type. Easy Mode assumes that you have your music library organized with each album\n");
    fmt::print("  in its own folder.\n");

    fmt::print("\n");
    fmt::print(COLOR_RED "Options:\n" COLOR_OFF);

    CMD_HELP("--help",     "-h", "Show this help");
    CMD_HELP("--quiet",      "-q",  "Don't print scanning status messages");
    fmt::print("\n");
    CMD_HELP("--multithread=n", "-m n", "Scan files with n parallel threads");
    CMD_HELP("--override=p", "-o p", "Load override settings from path p");

    fmt::print("\n");

    fmt::print("Please report any issues to " PROJECT_URL "/issues\n");
    fmt::print("\n");
}


static inline void help_custom(void) {
    fmt::print(COLOR_RED "Usage: " COLOR_OFF "{}{}{} custom [OPTIONS] FILES...\n", COLOR_GREEN, EXECUTABLE_TITLE, COLOR_OFF);

    fmt::print("  Custom Mode allows the user to specify the options to scan the files with. The\n");
    fmt::print("  list of files to scan must be listed explicitly after the options.\n");
    fmt::print("\n");
    
    fmt::print(COLOR_RED "Options:\n" COLOR_OFF);
    CMD_HELP("--help",     "-h", "Show this help");
    fmt::print("\n");

    CMD_HELP("--album",  "-a", "Calculate album gain and peak");
    fmt::print("\n");

    CMD_HELP("--tagmode=s", "-s s", "Scan files but don't write ReplayGain tags (default)");
    CMD_HELP("--tagmode=d", "-s d",  "Delete ReplayGain tags from files");
    CMD_HELP("--tagmode=i", "-s i",  "Scan and write ReplayGain 2.0 tags to files");
    fmt::print("\n");

    CMD_HELP("--loudness=n",  "-l n",  "Use n LUFS as target loudness (-30 ≤ n ≤ -5)");
    fmt::print("\n");

    CMD_HELP("--clip-mode n", "-c n", "No clipping protection (default)");
    CMD_HELP("--clip-mode p", "-c p", "Clipping protection enabled for positive gain values only");
    CMD_HELP("--clip-mode a", "-c a", "Clipping protection always enabled");
    CMD_HELP("--max-peak=n", "-K n", "Use max peak level n dB for clipping protection");
    CMD_HELP("--true-peak",  "-t", "Use true peak for peak calculations");

    fmt::print("\n");

    CMD_HELP("--lowercase", "-L", "Force lowercase tags (MP2/MP3/MP4/WMA/WAV/AIFF)");
    CMD_CONT("This is non-standard but sometimes needed");
    CMD_HELP("--strip-tags", "-S", "Strip tag types other than ID3v2 from MP2/MP3");
    CMD_CONT("Strip tag types other than APEv2 from WavPack/APE");
    CMD_HELP("--id3v2-version=3", "-I 3", "Write ID3v2.3 tags to MP2/MP3/WAV/AIFF");
    CMD_HELP("--id3v2-version=4", "-I 4", "Write ID3v2.4 tags to MP2/MP3/WAV/AIFF (default)");

    fmt::print("\n");

    CMD_HELP("--output", "-O",  "Database-friendly tab-delimited list output");
    CMD_HELP("--quiet",      "-q",  "Don't print scanning status messages");

    fmt::print("\n");

    fmt::print("Please report any issues to " PROJECT_URL "/issues\n");
    fmt::print("\n");
}


static void version(void) {
    int  ebur128_v_major     = 0;
    int  ebur128_v_minor     = 0;
    int  ebur128_v_patch     = 0;
    unsigned swr_ver         = 0;
    unsigned lavf_ver        = 0;
    unsigned lavc_ver        = 0;
    unsigned lavu_ver        = 0;
    std::string ebur128_version;
    std::string swr_version;
    std::string lavf_version;
    std::string lavc_version;
    std::string lavu_version;
    std::string taglib_version;

    // libebur128 version check
    ebur128_get_version(&ebur128_v_major, &ebur128_v_minor, &ebur128_v_patch);
    ebur128_version = fmt::format("{}.{}.{}", ebur128_v_major, ebur128_v_minor, ebur128_v_patch);

    // libavformat version
    lavf_ver = avformat_version();
    lavf_version = fmt::format("{}.{}.{}", lavf_ver>>16, lavf_ver>>8&0xff, lavf_ver&0xff);

    // libavcodec version
    lavc_ver = avcodec_version();
    lavc_version = fmt::format("{}.{}.{}", lavc_ver>>16, lavc_ver>>8&0xff, lavc_ver&0xff);

    // libavcodec version
    lavu_ver = avutil_version();
    lavu_version = fmt::format("{}.{}.{}", lavu_ver>>16, lavu_ver>>8&0xff, lavu_ver&0xff);

    // libswresample version
    swr_ver = swresample_version();
    swr_version = fmt::format("{}.{}.{}", swr_ver>>16, swr_ver>>8&0xff, swr_ver&0xff);

    // taglib version
    taglib_get_version(taglib_version);

    // Print versions
    fmt::print(COLOR_GREEN PROJECT_NAME COLOR_OFF " " PROJECT_VERSION " - using:\n");
    PRINT_LIB("libebur128", ebur128_version);
    PRINT_LIB("libavformat", lavf_version);
    PRINT_LIB("libavcodec", lavc_version);
    PRINT_LIB("libavutil", lavu_version);
    PRINT_LIB("libswresample", swr_version);
    fmt::print("\n");
    fmt::print("Built with:\n");
    PRINT_LIB("taglib", taglib_version);
    fmt::print("\n");
#ifdef __GNUC__
    fmt::print(COLOR_YELLOW "Compiler:" COLOR_OFF "   GCC {}.{}\n", __GNUC__, __GNUC_MINOR__);
#endif

#ifdef __clang__
    fmt::print(COLOR_YELLOW "Compiler:" COLOR_OFF "   Clang {}.{}.{}\n", __clang_major__, __clang_minor__, __clang_patchlevel__);
#endif

#ifdef _MSC_VER
    fmt::print(COLOR_YELLOW "Compiler:" COLOR_OFF "   Microsoft C/C++ {:.2f}\n", (float) _MSC_VER / 100.0f);
#endif

    fmt::print(COLOR_YELLOW "Build date:" COLOR_OFF " " __DATE__ "\n");
}