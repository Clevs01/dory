/* <dory/compress/compression_codec_api.cc>

   ----------------------------------------------------------------------------
   Copyright 2017 Dave Peterson <dave@dspeterson.com>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
   ----------------------------------------------------------------------------

   Implements <dory/compress/compression_init.h>.
 */

#include <dory/compress/compression_codec_api.h>

#include <dory/compress/get_compression_codec.h>

using namespace Base;
using namespace Dory;
using namespace Dory::Compress;

int TCompressionCodecApi::CompressionLevelParam(
    const TOpt<int> &requested_level) const noexcept {
  assert(this);
  auto real_level = GetRealCompressionLevel(requested_level);
  return (real_level.IsKnown()) ? *real_level : 0;
}
