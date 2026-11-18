/*
 * LCG2 -- SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Michele Giugliano
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * stimgen_api.h -- the part of StimGen 2's library interface that LCG2 uses.
 *
 * These are the prototypes of the sg_api_* functions of the StimGen version
 * named in STIMGEN_PIN (see sg.h and sg_api.c in StimGen's src/).  They are
 * written out here, and sg.h is deliberately NOT included, so that nothing but
 * this interface can be called by mistake: the other StimGen functions call
 * exit() on an error, which must never happen while the outputs of a board are
 * live.  The sg_api_* functions set an error trap instead: on an error they
 * return -1 and sg_api_error() has the message.
 *
 * If StimGen changes one of these, the pin check stops the build, and this
 * file is the one to compare.
 *
 * Rules of the interface (from sg_api.c):
 *   - every function returns 0 on success and -1 on error;
 *   - results (sg_api_text, sg_api_result, sg_api_warnings, sg_api_error) are
 *     valid only until the next call: copy them out at once;
 *   - an empty string means "not given" for the rate, the seed and the unit;
 *   - the text is read as a file in the current directory, so 'use' and
 *     'file()' find the files of the current directory.
 */
#ifndef LCG2_STIMGEN_API_H
#define LCG2_STIMGEN_API_H

#include <stddef.h>
#include <stdint.h>

/* The complete .sgb file of one stimulus (or one trial of a protocol: then
 * trial_json and protocol_text fill its provenance record). */
int sg_api_render_trial(const char *text, const char *rate, const char *seed,
                        const char *unit, const char *trial_json,
                        const char *protocol_text);

/* The canonical form of a stimulus or plain waveform, in sg_api_text(): the same
 * stimulus written in one fixed way, with every `use` expanded, so that it is
 * self-contained.  (A protocol is not accepted.) */
int sg_api_canon(const char *text, const char *unit);

/* "waveform", "stimulus", "protocol" or "index", in sg_api_text(). */
int sg_api_kind(const char *text);

/* The trials of a protocol as JSON, in sg_api_text(). */
int sg_api_expand(const char *text, const char *seed);

const char *sg_api_error(void);
const char *sg_api_text(void);
const char *sg_api_warnings(void);       /* a JSON array of strings */
const uint8_t *sg_api_result(void);
size_t sg_api_result_size(void);
const char *sg_api_version(void);

#endif /* LCG2_STIMGEN_API_H */
