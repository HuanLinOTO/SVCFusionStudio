#include "SHA256Utils.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace {

constexpr std::array<std::uint32_t, 64> kTable = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

inline std::uint32_t rotr(std::uint32_t x, std::uint32_t n) {
  return (x >> n) | (x << (32u - n));
}

struct Sha256Ctx {
  std::array<std::uint32_t, 8> state = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                        0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                        0x1f83d9abu, 0x5be0cd19u};
  std::array<std::uint8_t, 64> block{};
  std::uint64_t totalBytes = 0;
  std::size_t blockUsed = 0;
};

void transform(Sha256Ctx &ctx, const std::uint8_t *chunk) {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    const int j = i * 4;
    w[i] = (static_cast<std::uint32_t>(chunk[j]) << 24) |
           (static_cast<std::uint32_t>(chunk[j + 1]) << 16) |
           (static_cast<std::uint32_t>(chunk[j + 2]) << 8) |
           static_cast<std::uint32_t>(chunk[j + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 =
        rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 =
        rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = ctx.state[0];
  std::uint32_t b = ctx.state[1];
  std::uint32_t c = ctx.state[2];
  std::uint32_t d = ctx.state[3];
  std::uint32_t e = ctx.state[4];
  std::uint32_t f = ctx.state[5];
  std::uint32_t g = ctx.state[6];
  std::uint32_t h = ctx.state[7];

  for (int i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t t1 = h + s1 + ch + kTable[static_cast<std::size_t>(i)] + w[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t t2 = s0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  ctx.state[0] += a;
  ctx.state[1] += b;
  ctx.state[2] += c;
  ctx.state[3] += d;
  ctx.state[4] += e;
  ctx.state[5] += f;
  ctx.state[6] += g;
  ctx.state[7] += h;
}

void update(Sha256Ctx &ctx, const std::uint8_t *data, std::size_t size) {
  if (size == 0)
    return;

  ctx.totalBytes += static_cast<std::uint64_t>(size);
  std::size_t pos = 0;

  while (pos < size) {
    const std::size_t available = 64u - ctx.blockUsed;
    const std::size_t n = std::min(available, size - pos);
    std::memcpy(ctx.block.data() + ctx.blockUsed, data + pos, n);
    ctx.blockUsed += n;
    pos += n;

    if (ctx.blockUsed == 64u) {
      transform(ctx, ctx.block.data());
      ctx.blockUsed = 0;
    }
  }
}

juce::String finalHex(Sha256Ctx &ctx) {
  const std::uint64_t bitLen = ctx.totalBytes * 8u;

  std::uint8_t pad = 0x80;
  update(ctx, &pad, 1);

  const std::uint8_t zero = 0x00;
  while (ctx.blockUsed != 56u)
    update(ctx, &zero, 1);

  std::uint8_t lenBytes[8];
  for (int i = 0; i < 8; ++i)
    lenBytes[7 - i] = static_cast<std::uint8_t>((bitLen >> (i * 8)) & 0xffu);
  update(ctx, lenBytes, 8);

  juce::String out;
  out.preallocateBytes(64);
  for (std::uint32_t v : ctx.state) {
    out << juce::String::formatted("%02x", static_cast<unsigned int>((v >> 24) & 0xff))
        << juce::String::formatted("%02x", static_cast<unsigned int>((v >> 16) & 0xff))
        << juce::String::formatted("%02x", static_cast<unsigned int>((v >> 8) & 0xff))
        << juce::String::formatted("%02x", static_cast<unsigned int>(v & 0xff));
  }
  return out.toLowerCase();
}

} // namespace

namespace SHA256Utils {

juce::String fileSHA256(const juce::File &file) {
  if (!file.existsAsFile())
    return {};

  std::unique_ptr<juce::InputStream> in(file.createInputStream());
  if (!in)
    return {};

  Sha256Ctx ctx;
  std::array<std::uint8_t, 8192> buffer{};
  for (;;) {
    const int n = in->read(buffer.data(), static_cast<int>(buffer.size()));
    if (n <= 0)
      break;
    update(ctx, buffer.data(), static_cast<std::size_t>(n));
  }

  return finalHex(ctx);
}

} // namespace SHA256Utils
