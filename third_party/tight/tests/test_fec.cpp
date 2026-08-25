#include "test_framework.hpp"

#include "tight/fec.hpp"

#include <algorithm>
#include <cstdint>

using namespace tight;

namespace {

Bytes make_frag(std::size_t size, std::uint8_t seed) {
    Bytes f(size);
    for (std::size_t i = 0; i < size; ++i)
        f[i] = static_cast<std::uint8_t>(seed + i * 7);
    return f;
}

} // namespace

TEST_CASE(fec_roundtrip_no_loss) {
    std::vector<Bytes> data = {make_frag(64, 1), make_frag(64, 2),
                               make_frag(64, 3), make_frag(64, 4)};
    auto parity = ReedSolomon::encode(data, 2, 64);
    CHECK_EQ(parity.size(), 2U);
    for (auto& p : parity) CHECK_EQ(p.size(), 64U);

    std::vector<std::optional<Bytes>> recv;
    for (auto& f : data) recv.push_back(f);
    CHECK(ReedSolomon::decode(recv, {}, 64));
    for (std::size_t i = 0; i < data.size(); ++i)
        CHECK(*recv[i] == data[i]);
}

TEST_CASE(fec_recovers_single_loss) {
    std::vector<Bytes> data = {make_frag(64, 10), make_frag(64, 20),
                               make_frag(64, 30), make_frag(64, 40)};
    auto parity = ReedSolomon::encode(data, 2, 64);

    std::vector<std::optional<Bytes>> recv = {data[0], std::nullopt, data[2],
                                              data[3]};
    std::vector<std::pair<std::size_t, Bytes>> par = {{0, parity[0]}};
    CHECK(ReedSolomon::decode(recv, par, 64));
    CHECK(recv[0] == data[0]);
    CHECK(recv[1] == data[1]);
    CHECK(recv[2] == data[2]);
    CHECK(recv[3] == data[3]);
}

TEST_CASE(fec_recovers_multiple_loss) {
    std::vector<Bytes> data = {make_frag(128, 5),  make_frag(128, 6),
                               make_frag(128, 7),  make_frag(128, 8),
                               make_frag(128, 9),  make_frag(128, 10),
                               make_frag(128, 11), make_frag(128, 12)};
    auto parity = ReedSolomon::encode(data, 3, 128);
    CHECK_EQ(parity.size(), 3U);

    std::vector<std::optional<Bytes>> recv = {
        data[0], std::nullopt, data[2], data[3],
        std::nullopt, data[5], data[6], std::nullopt};
    std::vector<std::pair<std::size_t, Bytes>> par = {
        {1, parity[1]}, {2, parity[2]}, {0, parity[0]}};
    CHECK(ReedSolomon::decode(recv, par, 128));
    for (std::size_t i = 0; i < data.size(); ++i) CHECK(*recv[i] == data[i]);
}

TEST_CASE(fec_insufficient_parity_fails) {
    std::vector<Bytes> data = {make_frag(32, 1), make_frag(32, 2),
                               make_frag(32, 3), make_frag(32, 4)};
    auto parity = ReedSolomon::encode(data, 1, 32);
    std::vector<std::optional<Bytes>> recv = {std::nullopt, std::nullopt, data[2],
                                              data[3]};
    std::vector<std::pair<std::size_t, Bytes>> par = {{0, parity[0]}};
    CHECK(!ReedSolomon::decode(recv, par, 32));
}

TEST_CASE(fec_parity_zero_is_xor) {
    std::vector<Bytes> data = {make_frag(50, 3), make_frag(50, 9),
                               make_frag(50, 17)};
    auto parity = ReedSolomon::encode(data, 1, 50);
    Bytes xor_all(50, 0);
    for (auto& f : data)
        for (std::size_t i = 0; i < 50; ++i) xor_all[i] ^= f[i];
    CHECK(parity[0] == xor_all);
}

TEST_CASE(fec_short_tail_zero_padding_equivalence) {
    // A short tail fragment must encode identically to its zero-padded form.
    std::vector<Bytes> data = {make_frag(64, 1), make_frag(64, 2)};
    data[1].resize(30); // short tail
    auto parity_short = ReedSolomon::encode(data, 2, 64);

    std::vector<Bytes> data_padded = {data[0], make_frag(64, 0)};
    data_padded[1].assign(64, 0); // zero padding
    data_padded[1][0] = data[1][0];
    data_padded[1][1] = data[1][1];
    std::vector<std::uint8_t> src = data[1];
    std::copy(src.begin(), src.end(), data_padded[1].begin());
    auto parity_padded = ReedSolomon::encode(data_padded, 2, 64);

    CHECK(parity_short == parity_padded);
}

TEST_CASE(fec_decode_no_missing_is_noop) {
    std::vector<Bytes> data = {make_frag(16, 1), make_frag(16, 2)};
    std::vector<std::optional<Bytes>> recv = {data[0], data[1]};
    CHECK(ReedSolomon::decode(recv, {}, 16));
    CHECK(*recv[0] == data[0]);
    CHECK(*recv[1] == data[1]);
}

TEST_CASE(fec_encode_into_reuses_buffer) {
    std::vector<Bytes> data = {make_frag(80, 1), make_frag(80, 2),
                               make_frag(80, 3)};
    std::vector<ReedSolomon::Span> spans;
    for (auto& f : data) spans.push_back({f.data(), f.size()});

    std::vector<Bytes> out;
    ReedSolomon::encode_into(spans, 2, 80, out);
    CHECK_EQ(out.size(), 2U);
    auto ref = ReedSolomon::encode(data, 2, 80);
    CHECK(out == ref);

    // Reuse: call again with different data, buffers must be overwritten.
    std::vector<Bytes> data2 = {make_frag(80, 100), make_frag(80, 101),
                                make_frag(80, 102)};
    std::vector<ReedSolomon::Span> spans2;
    for (auto& f : data2) spans2.push_back({f.data(), f.size()});
    ReedSolomon::encode_into(spans2, 2, 80, out);
    CHECK(out == ReedSolomon::encode(data2, 2, 80));
}

TEST_CASE(fec_large_fragment_count) {
    std::vector<Bytes> data;
    for (std::size_t i = 0; i < 20; ++i) data.push_back(make_frag(1300, static_cast<std::uint8_t>(i)));
    auto parity = ReedSolomon::encode(data, 4, 1300);
    std::vector<std::optional<Bytes>> recv;
    for (std::size_t i = 0; i < 20; ++i)
        recv.push_back(i == 3 || i == 7 || i == 11 || i == 19 ? std::nullopt
                                                              : std::optional<Bytes>(data[i]));
    std::vector<std::pair<std::size_t, Bytes>> par = {{0, parity[0]}, {1, parity[1]},
                                                      {2, parity[2]}, {3, parity[3]}};
    CHECK(ReedSolomon::decode(recv, par, 1300));
    for (std::size_t i = 0; i < 20; ++i) CHECK(*recv[i] == data[i]);
}
