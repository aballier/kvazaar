/*****************************************************************************
 * This file is part of Kvazaar HEVC encoder.
 *
 * Copyright (C) 2013-2015 Tampere University of Technology and others (see
 * COPYING file).
 *
 * Kvazaar is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * Kvazaar is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with Kvazaar.  If not, see <http://www.gnu.org/licenses/>.
 ****************************************************************************/

/*
 * \file
 *
 */

#ifdef _WIN32
/* The following two defines must be located before the inclusion of any system header files. */
#define WINVER       0x0500
#define _WIN32_WINNT 0x0500
#include <fcntl.h>    /* _O_BINARY */
#include <io.h>       /* _setmode() */
#endif

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> // IWYU pragma: keep for CLOCKS_PER_SEC

#include "checkpoint.h"
#include "cli.h"
#include "encoder.h"
#include "global.h" // IWYU pragma: keep
#include "kvazaar.h"
#include "kvazaar_internal.h"
#include "threads.h"
#include "yuv_io.h"

/**
 * \brief Open a file for reading.
 *
 * If the file is "-", stdin is used.
 *
 * \param filename  name of the file to open or "-"
 * \return          the opened file or NULL if opening fails
 */
static FILE* open_input_file(const char* filename)
{
  if (!strcmp(filename, "-")) return stdin;
  return fopen(filename, "rb");
}

/**
 * \brief Open a file for writing.
 *
 * If the file is "-", stdout is used.
 *
 * \param filename  name of the file to open or "-"
 * \return          the opened file or NULL if opening fails
 */
static FILE* open_output_file(const char* filename)
{
  if (!strcmp(filename, "-")) return stdout;
  return fopen(filename, "wb");
}

static unsigned get_padding(unsigned width_or_height){
  if (width_or_height % CU_MIN_SIZE_PIXELS){
    return CU_MIN_SIZE_PIXELS - (width_or_height % CU_MIN_SIZE_PIXELS);
  }else{
    return 0;
  }
}

#if KVZ_BIT_DEPTH == 8
#define PSNRMAX (255.0 * 255.0)
#else
  #define PSNRMAX ((double)PIXEL_MAX * (double)PIXEL_MAX)
#endif

/**
 * \brief Calculates image PSNR value
 *
 * \param src   source picture
 * \param rec   reconstructed picture
 * \prama psnr  returns the PSNR
 */
static void compute_psnr(const kvz_picture *const src,
                         const kvz_picture *const rec,
                         double psnr[3])
{
  assert(src->width  == rec->width);
  assert(src->height == rec->height);

  int32_t pixels = src->width * src->height;
  int colors = rec->chroma_format == KVZ_CSP_400 ? 1 : 3;

  for (int32_t c = 0; c < colors; ++c) {
    int32_t num_pixels = pixels;
    if (c != COLOR_Y) {
      num_pixels >>= 2;
    }
    psnr[c] = 0;
    for (int32_t i = 0; i < num_pixels; ++i) {
      const int32_t error = src->data[c][i] - rec->data[c][i];
      psnr[c] += error * error;
    }

    // Avoid division by zero
    if (psnr[c] == 0) psnr[c] = 99.0;
    psnr[c] = 10 * log10((num_pixels * PSNRMAX) / ((double)psnr[c]));;
  }
}

typedef struct {
  // Mutexes for synchronization.
  pthread_mutex_t* input_mutex;
  pthread_mutex_t* main_thread_mutex;

  // Parameters passed from main thread to input thread.
  FILE* input;
  const kvz_api *api;
  const cmdline_opts_t *opts;
  const encoder_control_t *encoder;
  const uint8_t padding_x;
  const uint8_t padding_y;

  // Picture and thread status passed from input thread to main thread.
  kvz_picture *img_in;
  int retval;
} input_handler_args;

#define PTHREAD_LOCK(l) if (pthread_mutex_lock((l)) != 0) { fprintf(stderr, "pthread_mutex_lock(%s) failed!\n", #l); assert(0); return 0; }
#define PTHREAD_UNLOCK(l) if (pthread_mutex_unlock((l)) != 0) { fprintf(stderr, "pthread_mutex_unlock(%s) failed!\n", #l); assert(0); return 0; }

#define RETVAL_RUNNING 0
#define RETVAL_FAILURE 1
#define RETVAL_EOF 2

/**
* \brief Handles input reading in a thread
*
* \param in_args  pointer to argument struct
*/
static void* input_read_thread(void* in_args)
{

  // Reading a frame works as follows:
  // - read full frame
  // if progressive: set read frame as output
  // if interlaced:
  // - allocate two fields and fill them according to field order
  // - deallocate the initial full frame

  input_handler_args* args = (input_handler_args*)in_args;
  kvz_picture *frame_in = NULL;
  int retval = RETVAL_RUNNING;
  int frames_read = 0;

  for (;;) {
    // Each iteration of this loop puts either a single frame or a field into
    // args->img_in for main thread to process.

    bool input_empty = !(args->opts->frames == 0 // number of frames to read is unknown
                         || frames_read < args->opts->frames); // not all frames have been read
    if (feof(args->input) || input_empty) {
      retval = RETVAL_EOF;
      goto done;
    }

    enum kvz_chroma_format csp = KVZ_FORMAT2CSP(args->opts->config->input_format);
    frame_in = args->api->picture_alloc_csp(csp,
                                            args->opts->config->width  + args->padding_x,
                                            args->opts->config->height + args->padding_y);

    if (!frame_in) {
      fprintf(stderr, "Failed to allocate image.\n");
      retval = RETVAL_FAILURE;
      goto done;
    }

    bool read_success = yuv_io_read(args->input, 
                                    args->opts->config->width,
                                    args->opts->config->height,
                                    args->encoder->cfg->input_bitdepth,
                                    args->encoder->bitdepth,
                                    frame_in);
    if (!read_success) {
      // reading failed
      if (feof(args->input)) {
        // When looping input, re-open the file and re-read data.
        if (args->opts->loop_input && args->input != stdin) {
          fclose(args->input);
          args->input = fopen(args->opts->input, "rb");
          if (args->input == NULL)
          {
            fprintf(stderr, "Could not re-open input file, shutting down!\n");
            retval = RETVAL_FAILURE;
            goto done;
          }
          bool read_success = yuv_io_read(args->input,
                                          args->opts->config->width,
                                          args->opts->config->height,
                                          args->encoder->cfg->input_bitdepth,
                                          args->encoder->bitdepth,
                                          frame_in);
          if (!read_success) {
            fprintf(stderr, "Could not re-open input file, shutting down!\n");
            retval = RETVAL_FAILURE;
            goto done;
          }
        } else {
          retval = RETVAL_EOF;
          goto done;
        }
      } else {
        fprintf(stderr, "Failed to read a frame %d\n", frames_read);
        retval = RETVAL_FAILURE;
        goto done;
      }
    }

    frames_read++;

    if (args->encoder->cfg->source_scan_type != 0) {
      // Set source scan type for frame, so that it will be turned into fields.
      frame_in->interlacing = args->encoder->cfg->source_scan_type;
    }

    // Wait until main thread is ready to receive the next frame.
    PTHREAD_LOCK(args->input_mutex);
    args->img_in = frame_in;
    args->retval = retval;
    // Unlock main_thread_mutex to notify main thread that the new img_in
    // and retval have been placed to args.
    PTHREAD_UNLOCK(args->main_thread_mutex);

    frame_in = NULL;
  }

done:
  // Wait until main thread is ready to receive the next frame.
  PTHREAD_LOCK(args->input_mutex);
  args->img_in = NULL;
  args->retval = retval;
  // Unlock main_thread_mutex to notify main thread that the new img_in
  // and retval have been placed to args.
  PTHREAD_UNLOCK(args->main_thread_mutex);

  // Do some cleaning up.
  args->api->picture_free(frame_in);

  pthread_exit(NULL);
  return 0;
}


/**
 * \brief Program main function.
 * \param argc Argument count from commandline
 * \param argv Argument list
 * \return Program exit state
 */
int main(int argc, char *argv[])
{
  int retval = EXIT_SUCCESS;

  cmdline_opts_t *opts = NULL; //!< Command line options
  kvz_encoder* enc = NULL;
  FILE *input  = NULL; //!< input file (YUV)
  FILE *output = NULL; //!< output file (HEVC NAL stream)
  FILE *recout = NULL; //!< reconstructed YUV output, --debug
  clock_t start_time = clock();
  clock_t encoding_start_cpu_time;
  KVZ_CLOCK_T encoding_start_real_time;
  
  clock_t encoding_end_cpu_time;
  KVZ_CLOCK_T encoding_end_real_time;

#ifdef _WIN32
  // Stderr needs to be text mode to convert \n to \r\n in Windows.
  setmode( _fileno( stderr ), _O_TEXT );
#endif
      
  CHECKPOINTS_INIT();

  const kvz_api * const api = kvz_api_get(8);

  opts = cmdline_opts_parse(api, argc, argv);
  // If problem with command line options, print banner and shutdown.
  if (!opts) {
    print_usage();

    goto exit_failure;
  }
  if (opts->version) {
    print_version();
    goto done;
  }
  if (opts->help) {
    print_help();
    goto done;
  }

  input = open_input_file(opts->input);
  if (input == NULL) {
    fprintf(stderr, "Could not open input file, shutting down!\n");
    goto exit_failure;
  }

  output = open_output_file(opts->output);
  if (output == NULL) {
    fprintf(stderr, "Could not open output file, shutting down!\n");
    goto exit_failure;
  }

#ifdef _WIN32
  // Set stdin and stdout to binary for pipes.
  if (input == stdin) {
    _setmode(_fileno(stdin), _O_BINARY);
  }
  if (output == stdout) {
    _setmode(_fileno(stdout), _O_BINARY);
  }
#endif

  if (opts->debug != NULL) {
    recout = open_output_file(opts->debug);
    if (recout == NULL) {
      fprintf(stderr, "Could not open reconstruction file (%s), shutting down!\n", opts->debug);
      goto exit_failure;
    }
  }

  enc = api->encoder_open(opts->config);
  if (!enc) {
    fprintf(stderr, "Failed to open encoder.\n");
    goto exit_failure;
  }

  encoder_control_t *encoder = enc->control;
  
  fprintf(stderr, "Input: %s, output: %s\n", opts->input, opts->output);
  fprintf(stderr, "  Video size: %dx%d (input=%dx%d)\n",
         encoder->in.width, encoder->in.height,
         encoder->in.real_width, encoder->in.real_height);

  if (opts->seek > 0 && !yuv_io_seek(input, opts->seek, opts->config->width, opts->config->height)) {
    fprintf(stderr, "Failed to seek %d frames.\n", opts->seek);
    goto exit_failure;
  }

  //Now, do the real stuff
  {

    KVZ_GET_TIME(&encoding_start_real_time);
    encoding_start_cpu_time = clock();

    uint64_t bitstream_length = 0;
    uint32_t frames_done = 0;
    double psnr_sum[3] = { 0.0, 0.0, 0.0 };

    uint8_t padding_x = get_padding(opts->config->width);
    uint8_t padding_y = get_padding(opts->config->height);

    pthread_t input_thread;

    pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t main_thread_mutex = PTHREAD_MUTEX_INITIALIZER;

    // Lock both mutexes at startup
    PTHREAD_LOCK(&main_thread_mutex);
    PTHREAD_LOCK(&input_mutex);

    // Give arguments via struct to the input thread
    input_handler_args in_args = {
      .input_mutex = NULL,
      .main_thread_mutex = NULL,

      .input = input,
      .api = api,
      .opts = opts,
      .encoder = encoder,
      .padding_x = padding_x,
      .padding_y = padding_y,

      .img_in = NULL,
      .retval = RETVAL_RUNNING,
    };
    in_args.input_mutex = &input_mutex;
    in_args.main_thread_mutex = &main_thread_mutex;

    if (pthread_create(&input_thread, NULL, input_read_thread, (void*)&in_args) != 0) {
      fprintf(stderr, "pthread_create failed!\n");
      assert(0);
      return 0;
    }
    kvz_picture *cur_in_img;
    for (;;) {

      // Skip mutex locking if the input thread does not exist.
      if (in_args.retval == RETVAL_RUNNING) {
        // Unlock input_mutex so that the input thread can write the new
        // img_in and retval to in_args.
        PTHREAD_UNLOCK(&input_mutex);
        // Wait until the input thread has updated in_args.
        PTHREAD_LOCK(&main_thread_mutex);

        cur_in_img = in_args.img_in;
        in_args.img_in = NULL;

      } else {
        cur_in_img = NULL;
      }

      if (in_args.retval == EXIT_FAILURE) {
        goto exit_failure;
      }

      kvz_data_chunk* chunks_out = NULL;
      kvz_picture *img_rec = NULL;
      kvz_picture *img_src = NULL;
      uint32_t len_out = 0;
      kvz_frame_info info_out;
      if (!api->encoder_encode(enc,
                               cur_in_img,
                               &chunks_out,
                               &len_out,
                               &img_rec,
                               &img_src,
                               &info_out)) {
        fprintf(stderr, "Failed to encode image.\n");
        api->picture_free(cur_in_img);
        goto exit_failure;
      }

      if (chunks_out == NULL && cur_in_img == NULL) {
        // We are done since there is no more input and output left.
        break;
      }

      if (chunks_out != NULL) {
        uint64_t written = 0;
        // Write data into the output file.
        for (kvz_data_chunk *chunk = chunks_out;
             chunk != NULL;
             chunk = chunk->next) {
          assert(written + chunk->len <= len_out);
          if (fwrite(chunk->data, sizeof(uint8_t), chunk->len, output) != chunk->len) {
            fprintf(stderr, "Failed to write data to file.\n");
            api->picture_free(cur_in_img);
            api->chunk_free(chunks_out);
            goto exit_failure;
          }
          written += chunk->len;
        }
        fflush(output);

        bitstream_length += len_out;

        // Compute and print stats.

        double frame_psnr[3] = { 0.0, 0.0, 0.0 };
        if (encoder->cfg->calc_psnr && encoder->cfg->source_scan_type == KVZ_INTERLACING_NONE) {
          // Do not compute PSNR for interlaced frames, because img_rec does not contain
          // the deinterlaced frame yet.
          compute_psnr(img_src, img_rec, frame_psnr);
        }

        if (recout) {
          // Since chunks_out was not NULL, img_rec should have been set.
          assert(img_rec);
          if (!yuv_io_write(recout,
                            img_rec,
                            opts->config->width,
                            opts->config->height)) {
            fprintf(stderr, "Failed to write reconstructed picture!\n");
          }
        }

        frames_done += 1;
        psnr_sum[0] += frame_psnr[0];
        psnr_sum[1] += frame_psnr[1];
        psnr_sum[2] += frame_psnr[2];

        print_frame_info(&info_out, frame_psnr, len_out);
      }

      api->picture_free(cur_in_img);
      api->chunk_free(chunks_out);
      api->picture_free(img_rec);
      api->picture_free(img_src);
    }

    KVZ_GET_TIME(&encoding_end_real_time);
    encoding_end_cpu_time = clock();
    // Coding finished

    // Print statistics of the coding
    fprintf(stderr, " Processed %d frames, %10llu bits",
            frames_done,
            (long long unsigned int)bitstream_length * 8);
    if (frames_done > 0) {
      fprintf(stderr, " AVG PSNR: %2.4f %2.4f %2.4f",
              psnr_sum[0] / frames_done,
              psnr_sum[1] / frames_done,
              psnr_sum[2] / frames_done);
    }
    fprintf(stderr, "\n");
    fprintf(stderr, " Total CPU time: %.3f s.\n", ((float)(clock() - start_time)) / CLOCKS_PER_SEC);

    {
      double encoding_time = ( (double)(encoding_end_cpu_time - encoding_start_cpu_time) ) / (double) CLOCKS_PER_SEC;
      double wall_time = KVZ_CLOCK_T_AS_DOUBLE(encoding_end_real_time) - KVZ_CLOCK_T_AS_DOUBLE(encoding_start_real_time);
      fprintf(stderr, " Encoding time: %.3f s.\n", encoding_time);
      fprintf(stderr, " Encoding wall time: %.3f s.\n", wall_time);
      fprintf(stderr, " Encoding CPU usage: %.2f%%\n", encoding_time/wall_time*100.f);
      fprintf(stderr, " FPS: %.2f\n", ((double)frames_done)/wall_time);
    }
    pthread_join(input_thread, NULL);
  }

  goto done;

exit_failure:
  retval = EXIT_FAILURE;

done:
  // deallocate structures
  if (enc) api->encoder_close(enc);
  if (opts) cmdline_opts_free(api, opts);

  // close files
  if (input)  fclose(input);
  if (output) fclose(output);
  if (recout) fclose(recout);

  CHECKPOINTS_FINALIZE();

  return retval;
}
