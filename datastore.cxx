/*
* rtl_power_fftw, program for calculating power spectrum from rtl-sdr reciever.
* Copyright (C) 2015 Klemen Blokar <klemen.blokar@ad-vega.si>
*                    Andrej Lajovic <andrej.lajovic@ad-vega.si>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "datastore.h"

Datastore::Datastore(int N_, int buf_length, int64_t repeats_, int buffers_) :
  N(N_), buffers(buffers_), repeats(repeats_),
  queue_histogram(buffers_+1, 0), pwr(N)
{
  for (int i = 0; i < buffers; i++)
    empty_buffers.push_back(new Buffer(buf_length));

  inbuf = (complex*)FFTW(alloc_complex)(N);
  outbuf = (complex*)FFTW(alloc_complex)(N);
  plan = FFTW(plan_dft_1d)(N, (fftw_cdt*)inbuf, (fftw_cdt*)outbuf,
                          FFTW_FORWARD, FFTW_MEASURE);
}

Datastore::~Datastore() {
  for (auto& buffer : empty_buffers)
    delete buffer;

  for (auto& buffer : occupied_buffers)
    delete buffer;

  FFTW(destroy_plan)(plan);
  FFTW(free)(inbuf);
  FFTW(free)(outbuf);
}

void fft(Datastore& data) {
  std::unique_lock<std::mutex>
    status_lock(data.status_mutex, std::defer_lock);
  int fft_pointer = 0;
  while (true) {
    // Wait until we have a bufferful of data
    status_lock.lock();
    while (data.occupied_buffers.empty() && !data.acquisition_finished)
      data.status_change.wait(status_lock);
    if (data.occupied_buffers.empty()) {
      // acquisition finished
      break;
    }
    Buffer& buffer(*data.occupied_buffers.front());
    data.occupied_buffers.pop_front();
    status_lock.unlock();
    //A neat new loop to avoid having to have data buffer aligned with fft buffer.
    unsigned int buffer_pointer = 0;
    while (buffer_pointer < buffer.size() && data.repeats_done < data.repeats ) {
      while (fft_pointer < data.N && buffer_pointer < buffer.size()) {
        //The magic aligment happens here: we have to change the phase of each next complex sample
        //by pi - this means that even numbered samples stay the same while odd numbered samples
        //get multiplied by -1 (thus rotated by pi in complex plane).
        //This gets us output spectrum shifted by half its size - just what we need to get the output right.
        const fft_datatype multiplier = (fft_pointer % 2 == 0 ? 1 : -1);
        complex bfr_val(buffer[buffer_pointer], buffer[buffer_pointer+1]);
        data.inbuf[fft_pointer] = (bfr_val - complex(127.0, 127.0)) * multiplier;
        buffer_pointer += 2;
        fft_pointer++;
      }
      if (fft_pointer == data.N) {
        FFTW(execute)(data.plan);
        for (int i = 0; i < data.N; i++) {
          data.pwr[i] += std::norm(data.outbuf[i]);
        }
        data.repeats_done++;
        fft_pointer = 0;
      }
    }

    status_lock.lock();
    data.empty_buffers.push_back(&buffer);
    data.status_change.notify_all();
    status_lock.unlock();
  }
}