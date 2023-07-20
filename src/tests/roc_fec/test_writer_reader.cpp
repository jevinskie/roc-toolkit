/*
 * Copyright (c) 2015 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <CppUTest/TestHarness.h>

#include "test_helpers/mock_allocator.h"
#include "test_helpers/packet_dispatcher.h"

#include "roc_core/buffer_factory.h"
#include "roc_core/heap_allocator.h"
#include "roc_core/scoped_ptr.h"
#include "roc_fec/codec_map.h"
#include "roc_fec/composer.h"
#include "roc_fec/headers.h"
#include "roc_fec/parser.h"
#include "roc_fec/reader.h"
#include "roc_fec/writer.h"
#include "roc_packet/interleaver.h"
#include "roc_packet/packet_factory.h"
#include "roc_packet/queue.h"
#include "roc_rtp/composer.h"
#include "roc_rtp/format_map.h"
#include "roc_rtp/headers.h"
#include "roc_rtp/parser.h"

namespace roc {
namespace fec {

namespace {

const size_t NumSourcePackets = 20;
const size_t NumRepairPackets = 10;

const unsigned SourceID = 555;
const unsigned PayloadType = rtp::PayloadType_L16_Stereo;

const size_t FECPayloadSize = 193;

const size_t MaxBuffSize = 500;

core::HeapAllocator allocator;
core::BufferFactory<uint8_t> buffer_factory(allocator, MaxBuffSize, true);
packet::PacketFactory packet_factory(allocator, true);

rtp::FormatMap format_map(allocator, true);
rtp::Parser rtp_parser(format_map, NULL);

Parser<RS8M_PayloadID, Source, Footer> rs8m_source_parser(&rtp_parser);
Parser<RS8M_PayloadID, Repair, Header> rs8m_repair_parser(NULL);
Parser<LDPC_Source_PayloadID, Source, Footer> ldpc_source_parser(&rtp_parser);
Parser<LDPC_Repair_PayloadID, Repair, Header> ldpc_repair_parser(NULL);

rtp::Composer rtp_composer(NULL);
Composer<RS8M_PayloadID, Source, Footer> rs8m_source_composer(&rtp_composer);
Composer<RS8M_PayloadID, Repair, Header> rs8m_repair_composer(NULL);
Composer<LDPC_Source_PayloadID, Source, Footer> ldpc_source_composer(&rtp_composer);
Composer<LDPC_Repair_PayloadID, Repair, Header> ldpc_repair_composer(NULL);

} // namespace

TEST_GROUP(writer_reader) {
    packet::PacketPtr source_packets[NumSourcePackets];

    CodecConfig codec_config;
    WriterConfig writer_config;
    ReaderConfig reader_config;

    void setup() {
        writer_config.n_source_packets = NumSourcePackets;
        writer_config.n_repair_packets = NumRepairPackets;
    }

    packet::IParser& source_parser() {
        switch (codec_config.scheme) {
        case packet::FEC_ReedSolomon_M8:
            return rs8m_source_parser;
        case packet::FEC_LDPC_Staircase:
            return ldpc_source_parser;
        default:
            roc_panic("bad scheme");
        }
    }

    packet::IParser& repair_parser() {
        switch (codec_config.scheme) {
        case packet::FEC_ReedSolomon_M8:
            return rs8m_repair_parser;
        case packet::FEC_LDPC_Staircase:
            return ldpc_repair_parser;
        default:
            roc_panic("bad scheme");
        }
    }

    packet::IComposer& source_composer() {
        switch (codec_config.scheme) {
        case packet::FEC_ReedSolomon_M8:
            return rs8m_source_composer;
        case packet::FEC_LDPC_Staircase:
            return ldpc_source_composer;
        default:
            roc_panic("bad scheme");
        }
    }

    packet::IComposer& repair_composer() {
        switch (codec_config.scheme) {
        case packet::FEC_ReedSolomon_M8:
            return rs8m_repair_composer;
        case packet::FEC_LDPC_Staircase:
            return ldpc_repair_composer;
        default:
            roc_panic("bad scheme");
        }
    }

    void recompose_packet(const packet::PacketPtr& p) {
        if (p->flags() & packet::Packet::FlagRepair) {
            CHECK(repair_composer().compose(*p));
        } else {
            CHECK(source_composer().compose(*p));
        }
    }

    void fill_all_packets(size_t sn) {
        for (size_t i = 0; i < NumSourcePackets; ++i) {
            source_packets[i] = fill_one_packet(sn + i);
        }
    }

    packet::PacketPtr fill_one_packet(size_t sn, size_t fec_payload_size = FECPayloadSize,
                                      packet::IComposer* composer = NULL) {
        CHECK(fec_payload_size > sizeof(rtp::Header));
        const size_t rtp_payload_size = fec_payload_size - sizeof(rtp::Header);

        packet::PacketPtr pp = packet_factory.new_packet();
        CHECK(pp);

        core::Slice<uint8_t> bp = buffer_factory.new_buffer();
        CHECK(bp);

        if (!composer) {
            composer = &source_composer();
        }
        CHECK(composer->prepare(*pp, bp, rtp_payload_size));

        pp->set_data(bp);

        UNSIGNED_LONGS_EQUAL(rtp_payload_size, pp->rtp()->payload.size());
        UNSIGNED_LONGS_EQUAL(fec_payload_size, pp->fec()->payload.size());

        pp->add_flags(packet::Packet::FlagAudio);

        pp->rtp()->source = SourceID;
        pp->rtp()->payload_type = PayloadType;
        pp->rtp()->seqnum = packet::seqnum_t(sn);
        pp->rtp()->timestamp = packet::timestamp_t(sn * 10);

        for (size_t i = 0; i < rtp_payload_size; i++) {
            pp->rtp()->payload.data()[i] = uint8_t(sn + i);
        }

        return pp;
    }

    void check_audio_packet(packet::PacketPtr pp, size_t sn,
                            size_t fec_payload_size = FECPayloadSize) {
        CHECK(fec_payload_size > sizeof(rtp::Header));
        const size_t rtp_payload_size = fec_payload_size - sizeof(rtp::Header);

        CHECK(pp);

        CHECK(pp->flags() & packet::Packet::FlagRTP);
        CHECK(pp->flags() & packet::Packet::FlagAudio);

        CHECK(pp->rtp());
        CHECK(pp->rtp()->header);
        CHECK(pp->rtp()->payload);

        UNSIGNED_LONGS_EQUAL(SourceID, pp->rtp()->source);

        UNSIGNED_LONGS_EQUAL(sn, pp->rtp()->seqnum);
        UNSIGNED_LONGS_EQUAL(packet::timestamp_t(sn * 10), pp->rtp()->timestamp);

        UNSIGNED_LONGS_EQUAL(PayloadType, pp->rtp()->payload_type);
        UNSIGNED_LONGS_EQUAL(rtp_payload_size, pp->rtp()->payload.size());

        for (size_t i = 0; i < rtp_payload_size; i++) {
            UNSIGNED_LONGS_EQUAL(uint8_t(sn + i), pp->rtp()->payload.data()[i]);
        }
    }

    void check_restored(packet::PacketPtr p, bool restored) {
        if (restored) {
            CHECK((p->flags() & packet::Packet::FlagRestored) != 0);
            CHECK(!p->fec());
        } else {
            CHECK((p->flags() & packet::Packet::FlagRestored) == 0);
            CHECK(p->fec());
        }
    }
};

TEST(writer_reader, no_losses) {
    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        fill_all_packets(0);

        for (size_t i = 0; i < NumSourcePackets; ++i) {
            writer.write(source_packets[i]);
        }
        dispatcher.push_stocks();

        UNSIGNED_LONGS_EQUAL(NumSourcePackets, dispatcher.source_size());
        UNSIGNED_LONGS_EQUAL(NumRepairPackets, dispatcher.repair_size());

        for (size_t i = 0; i < NumSourcePackets; ++i) {
            packet::PacketPtr p = reader.read();
            CHECK(p);
            check_audio_packet(p, i);
            check_restored(p, false);
        }
    }
}

TEST(writer_reader, 1_loss) {
    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        fill_all_packets(0);

        dispatcher.lose(11);

        for (size_t i = 0; i < NumSourcePackets; ++i) {
            writer.write(source_packets[i]);
        }
        dispatcher.push_stocks();

        LONGS_EQUAL(NumSourcePackets - 1, dispatcher.source_size());
        LONGS_EQUAL(NumRepairPackets, dispatcher.repair_size());

        for (size_t i = 0; i < NumSourcePackets; ++i) {
            packet::PacketPtr p = reader.read();
            CHECK(p);
            check_audio_packet(p, i);
            check_restored(p, i == 11);
        }
    }
}

TEST(writer_reader, lost_first_packet_in_first_block) {
    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        // Sending first block except first packet.
        fill_all_packets(0);
        dispatcher.lose(0);
        for (size_t i = 0; i < NumSourcePackets; ++i) {
            writer.write(source_packets[i]);
        }

        // Sending second block lossless.
        dispatcher.clear_losses();
        fill_all_packets(NumSourcePackets);
        for (size_t i = 0; i < NumSourcePackets; ++i) {
            writer.write(source_packets[i]);
        }
        dispatcher.push_stocks();

        // Receive every sent packet except the first one.
        for (size_t i = 1; i < NumSourcePackets * 2; ++i) {
            packet::PacketPtr p = reader.read();
            if (i < NumSourcePackets) {
                CHECK(!reader.is_started());
            } else {
                CHECK(reader.is_started());
            }
            check_audio_packet(p, i);
            check_restored(p, false);
        }
        CHECK(dispatcher.source_size() == 0);
    }
}

TEST(writer_reader, lost_one_source_and_all_repair_packets) {
    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        // Send first block without one source and all repair packets.
        dispatcher.lose(3);
        for (size_t i = 0; i < NumRepairPackets; ++i) {
            dispatcher.lose(NumSourcePackets + i);
        }
        fill_all_packets(0);
        for (size_t i = 0; i < NumSourcePackets; ++i) {
            writer.write(source_packets[i]);
        }
        dispatcher.push_stocks();

        // Send second block without one source packet.
        dispatcher.clear_losses();
        dispatcher.lose(5);
        fill_all_packets(NumSourcePackets);
        for (size_t i = 0; i < NumSourcePackets; ++i) {
            writer.write(source_packets[i]);
        }
        dispatcher.push_stocks();

        // Receive packets.
        for (size_t i = 0; i < NumSourcePackets * 2; ++i) {
            if (i == 3) {
                // nop
            } else if (i == NumSourcePackets + 5) {
                packet::PacketPtr p = reader.read();
                CHECK(p);
                check_audio_packet(p, i);
                check_restored(p, true);
            } else {
                packet::PacketPtr p = reader.read();
                CHECK(p);
                check_audio_packet(p, i);
                check_restored(p, false);
            }
        }

        CHECK(dispatcher.source_size() == 0);
    }
}

TEST(writer_reader, multiple_blocks_1_loss) {
    enum { NumBlocks = 40 };

    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        for (size_t block_num = 0; block_num < NumBlocks; ++block_num) {
            size_t lost_sq = size_t(-1);
            if (block_num != 5 && block_num != 21 && block_num != 22) {
                lost_sq = (block_num + 1) % (NumSourcePackets + NumRepairPackets);
                dispatcher.lose(lost_sq);
            }

            fill_all_packets(NumSourcePackets * block_num);

            for (size_t i = 0; i < NumSourcePackets; ++i) {
                writer.write(source_packets[i]);
            }
            dispatcher.push_stocks();

            if (lost_sq == size_t(-1)) {
                CHECK(dispatcher.source_size() == NumSourcePackets);
                CHECK(dispatcher.repair_size() == NumRepairPackets);
            } else if (lost_sq < NumSourcePackets) {
                CHECK(dispatcher.source_size() == NumSourcePackets - 1);
                CHECK(dispatcher.repair_size() == NumRepairPackets);
            } else {
                CHECK(dispatcher.source_size() == NumSourcePackets);
                CHECK(dispatcher.repair_size() == NumRepairPackets - 1);
            }

            for (size_t i = 0; i < NumSourcePackets; ++i) {
                packet::PacketPtr p = reader.read();
                CHECK(p);

                check_audio_packet(p, NumSourcePackets * block_num + i);

                if (lost_sq == size_t(-1)) {
                    check_restored(p, false);
                } else {
                    check_restored(p,
                                   i == lost_sq % (NumSourcePackets + NumRepairPackets));
                }
            }

            dispatcher.reset();
        }
    }
}

TEST(writer_reader, multiple_blocks_in_queue) {
    enum { NumBlocks = 3 };

    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        for (size_t block_num = 0; block_num < NumBlocks; ++block_num) {
            fill_all_packets(NumSourcePackets * block_num);

            for (size_t i = 0; i < NumSourcePackets; ++i) {
                writer.write(source_packets[i]);
            }
        }
        dispatcher.push_stocks();

        CHECK(dispatcher.source_size() == NumSourcePackets * NumBlocks);
        CHECK(dispatcher.repair_size() == NumRepairPackets * NumBlocks);

        for (size_t block_num = 0; block_num < NumBlocks; ++block_num) {
            for (size_t i = 0; i < NumSourcePackets; ++i) {
                packet::PacketPtr p = reader.read();
                CHECK(p);
                check_audio_packet(p, NumSourcePackets * block_num + i);
                check_restored(p, false);
            }

            dispatcher.reset();
        }
    }
}

TEST(writer_reader, interleaved_packets) {
    enum { NumPackets = NumSourcePackets * 30 };

    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        packet::Interleaver intrlvr(dispatcher, allocator, 10);

        CHECK(intrlvr.is_valid());

        Writer writer(writer_config, codec_config.scheme, *encoder, intrlvr,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        packet::PacketPtr many_packets[NumPackets];

        for (size_t i = 0; i < NumPackets; ++i) {
            many_packets[i] = fill_one_packet(i);
            writer.write(many_packets[i]);
        }
        dispatcher.push_stocks();

        intrlvr.flush();

        for (size_t i = 0; i < NumPackets; ++i) {
            packet::PacketPtr p = reader.read();
            CHECK(p);
            check_audio_packet(p, i);
            check_restored(p, false);
        }
    }
}

TEST(writer_reader, delayed_packets) {
    // 1. Deliver first half of block.
    // 2. Read first half of block.
    // 3. Try to read more and get NULL.
    // 4. Deliver second half of block.
    // 5. Read second half of block.
    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        fill_all_packets(0);

        for (size_t i = 0; i < NumSourcePackets; ++i) {
            writer.write(source_packets[i]);
        }

        CHECK(NumSourcePackets > 10);

        // deliver 10 packets to reader
        for (size_t i = 0; i < 10; ++i) {
            dispatcher.push_source_stock(1);
        }

        // read 10 packets
        for (size_t i = 0; i < 10; ++i) {
            packet::PacketPtr p = reader.read();
            CHECK(p);
            check_audio_packet(p, i);
            check_restored(p, false);
        }

        // the rest packets are "delayed" and were not delivered to reader
        // try to read 11th packet and get NULL
        CHECK(!reader.read());

        // deliver "delayed" packets
        dispatcher.push_stocks();

        // successfully read packets starting from the 11th packet
        for (size_t i = 10; i < NumSourcePackets; ++i) {
            packet::PacketPtr p = reader.read();
            CHECK(p);
            check_audio_packet(p, i);
            check_restored(p, false);
        }
    }
}

TEST(writer_reader, late_out_of_order_packets) {
    // 1. Send a block, but delay some packets in the middle of the block.
    // 2. Read first part of the block before delayed packets.
    // 3. Deliver all delayed packets except one.
    // 4. Read second part of the block.
    // 5. Deliver the last delayed packet.
    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        fill_all_packets(0);

        // Mark packets 7-10 as delayed
        dispatcher.clear_delays();
        for (size_t i = 7; i <= 10; ++i) {
            dispatcher.delay(i);
        }

        for (size_t i = 0; i < NumSourcePackets; ++i) {
            writer.write(source_packets[i]);
        }

        // Deliver packets 0-6 and 11-20
        dispatcher.push_stocks();
        CHECK(dispatcher.source_size() == NumSourcePackets - (10 - 7 + 1));

        // Read packets 0-6
        for (size_t i = 0; i < 7; ++i) {
            packet::PacketPtr p = reader.read();
            CHECK(p);
            check_audio_packet(p, i);
            check_restored(p, false);
        }

        // Deliver packets 7-9
        dispatcher.push_delayed(7);
        dispatcher.push_delayed(8);
        dispatcher.push_delayed(9);

        for (size_t i = 7; i < NumSourcePackets; ++i) {
            packet::PacketPtr p = reader.read();
            CHECK(p);
            check_audio_packet(p, i);

            // packets 7-9 were out of order but not late and should be read
            // packet 10 was out of order and late and should be repaired
            check_restored(p, i == 10);

            // Deliver packet 10 (reader should throw it away)
            if (i == 10) {
                dispatcher.push_delayed(10);
            }
        }

        LONGS_EQUAL(0, dispatcher.source_size());
    }
}

TEST(writer_reader, repair_packets_before_source_packets) {
    writer_config.n_source_packets = 30;
    writer_config.n_repair_packets = 40;

    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, writer_config.n_source_packets,
                                          writer_config.n_repair_packets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        packet::seqnum_t wr_sn = 0;
        packet::seqnum_t rd_sn = 0;

        // Encode first block.
        for (size_t i = 0; i < writer_config.n_source_packets; ++i) {
            writer.write(fill_one_packet(wr_sn));
            wr_sn++;
        }

        // Deliver first block.
        dispatcher.push_stocks();

        // Read first block.
        for (size_t i = 0; i < writer_config.n_source_packets; ++i) {
            packet::PacketPtr p = reader.read();
            CHECK(p);
            check_audio_packet(p, rd_sn);
            check_restored(p, false);
            rd_sn++;
        }

        // Encode second block.
        for (size_t i = 0; i < writer_config.n_source_packets; ++i) {
            writer.write(fill_one_packet(wr_sn));
            wr_sn++;
        }

        // Deliver repair packets from second block.
        dispatcher.push_repair_stock(writer_config.n_repair_packets);

        // Read second block.
        for (size_t i = 0; i < writer_config.n_source_packets; ++i) {
            packet::PacketPtr p = reader.read();
            CHECK(p);

            // All packets should be restored.
            check_audio_packet(p, rd_sn);
            check_restored(p, true);

            rd_sn++;

            if (i == 0) {
                // Deliver source packets from second block.
                // These packets should be dropped.
                dispatcher.push_stocks();
            }
        }

        CHECK(dispatcher.source_size() == 0);
        CHECK(dispatcher.repair_size() == 0);

        UNSIGNED_LONGS_EQUAL(wr_sn, rd_sn);
    }
}

TEST(writer_reader, repair_packets_mixed_with_source_packets) {
    writer_config.n_source_packets = 30;
    writer_config.n_repair_packets = 40;

    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, writer_config.n_source_packets,
                                          writer_config.n_repair_packets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        packet::seqnum_t wr_sn = 0;
        packet::seqnum_t rd_sn = 0;

        // Encode first block.
        for (size_t i = 0; i < writer_config.n_source_packets; ++i) {
            writer.write(fill_one_packet(wr_sn));
            wr_sn++;
        }

        // Deliver first block.
        dispatcher.push_stocks();

        // Read first block.
        for (size_t i = 0; i < writer_config.n_source_packets; ++i) {
            packet::PacketPtr p = reader.read();
            CHECK(p);
            check_audio_packet(p, rd_sn);
            check_restored(p, false);
            rd_sn++;
        }

        // Lose all source packets except first and last 5 packets.
        for (size_t i = 5; i < writer_config.n_source_packets - 5; ++i) {
            dispatcher.lose(i);
        }

        // Encode second block.
        for (size_t i = 0; i < writer_config.n_source_packets; ++i) {
            writer.write(fill_one_packet(wr_sn));
            wr_sn++;
        }

        // Deliver some repair packets.
        dispatcher.push_repair_stock(3);

        // Delivered repair packets should not be enough for restore.
        CHECK(!reader.read());

        // Deliver first and last 5 source packets.
        dispatcher.push_source_stock(10);

        // Read second block.
        for (size_t i = 0; i < writer_config.n_source_packets; ++i) {
            packet::PacketPtr p = reader.read();
            CHECK(p);

            // All packets except first and last 5 should be restored.
            check_audio_packet(p, rd_sn);
            check_restored(p, i >= 5 && i < writer_config.n_source_packets - 5);

            rd_sn++;

            if (i == 0) {
                // Deliver the rest repair pacekets.
                dispatcher.push_repair_stock(writer_config.n_repair_packets - 3);
            }
        }

        CHECK(dispatcher.source_size() == 0);
        CHECK(dispatcher.repair_size() == 0);

        UNSIGNED_LONGS_EQUAL(wr_sn, rd_sn);
    }
}

TEST(writer_reader, multiple_repair_attempts) {
    // 1. Lose two distant packets and hold every fec packets in first block,
    //    receive second full block.
    // 2. Detect first loss.
    // 3. Transmit fec packets.
    // 4. Check remaining data packets including lost one.
    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        fill_all_packets(0);

        dispatcher.lose(5);
        dispatcher.lose(15);

        for (size_t i = 0; i < NumSourcePackets; ++i) {
            writer.write(source_packets[i]);
            if (i != 5 && i != 15) {
                dispatcher.push_source_stock(1);
            }
        }

        dispatcher.clear_losses();

        fill_all_packets(NumSourcePackets);
        for (size_t i = 0; i < NumSourcePackets; ++i) {
            writer.write(source_packets[i]);
            dispatcher.push_source_stock(1);
        }

        for (size_t i = 0; i < NumSourcePackets; ++i) {
            if (i != 5 && i != 15) {
                packet::PacketPtr p = reader.read();
                CHECK(p);
                check_audio_packet(p, i);
                check_restored(p, false);
            } else if (i == 15) {
                // The moment of truth. Deliver FEC packets accumulated in dispatcher.
                // Reader must try to decode once more.
                dispatcher.push_stocks();

                packet::PacketPtr p = reader.read();
                CHECK(p);
                check_audio_packet(p, i);
                check_restored(p, true);
            } else if (i == 5) {
                // nop
            }
        }

        for (size_t i = 0; i < NumSourcePackets; ++i) {
            packet::PacketPtr p = reader.read();
            CHECK(p);
            check_audio_packet(p, i + NumSourcePackets);
            check_restored(p, false);
        }

        LONGS_EQUAL(0, dispatcher.source_size());
    }
}

TEST(writer_reader, drop_outdated_block) {
    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        // Send first block.
        fill_all_packets(NumSourcePackets);
        for (size_t n = 0; n < NumSourcePackets; ++n) {
            writer.write(source_packets[n]);
        }

        // Send outdated block.
        fill_all_packets(0);
        for (size_t n = 0; n < NumSourcePackets; ++n) {
            writer.write(source_packets[n]);
        }

        // Send next block.
        fill_all_packets(NumSourcePackets * 2);
        for (size_t n = 0; n < NumSourcePackets; ++n) {
            writer.write(source_packets[n]);
        }

        dispatcher.push_stocks();

        // Read first block.
        const packet::PacketPtr first_packet = reader.read();
        CHECK(first_packet);

        const packet::blknum_t sbn = first_packet->fec()->source_block_number;

        for (size_t n = 1; n < NumSourcePackets; ++n) {
            const packet::PacketPtr p = reader.read();
            CHECK(p);

            CHECK(p->fec()->source_block_number == sbn);
        }

        // Read second block.
        for (size_t n = 0; n < NumSourcePackets; ++n) {
            const packet::PacketPtr p = reader.read();
            CHECK(p);

            CHECK(p->fec()->source_block_number == sbn + 1);
        }
    }
}

TEST(writer_reader, repaired_block_numbering) {
    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        const size_t lost_packet_n = 7;

        // Write first block lossy.
        fill_all_packets(0);
        dispatcher.lose(lost_packet_n);

        for (size_t n = 0; n < NumSourcePackets; ++n) {
            writer.write(source_packets[n]);
        }

        dispatcher.clear_losses();

        // Write second block lossless.
        fill_all_packets(NumSourcePackets);

        for (size_t n = 0; n < NumSourcePackets; ++n) {
            writer.write(source_packets[n]);
        }

        dispatcher.push_stocks();

        // Read first block.
        const packet::PacketPtr first_packet = reader.read();
        CHECK(first_packet);

        const packet::blknum_t sbn = first_packet->fec()->source_block_number;

        for (size_t n = 1; n < NumSourcePackets; ++n) {
            const packet::PacketPtr p = reader.read();
            CHECK(p);

            check_audio_packet(p, n);
            check_restored(p, n == lost_packet_n);

            if (n != lost_packet_n) {
                CHECK(p->fec());
                CHECK(p->fec()->source_block_number == sbn);
            } else {
                CHECK(!p->fec());
            }
        }

        // Read second block.
        for (size_t n = 0; n < NumSourcePackets; ++n) {
            const packet::PacketPtr p = reader.read();
            CHECK(p);

            check_audio_packet(p, NumSourcePackets + n);
            check_restored(p, false);

            CHECK(p->fec());
            CHECK(p->fec()->source_block_number == sbn + 1);
        }
    }
}

TEST(writer_reader, invalid_esi) {
    enum { NumBlocks = 5 };

    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        packet::Queue queue;

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, queue,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        for (size_t n_block = 0; n_block < NumBlocks; n_block++) {
            fill_all_packets(0);

            // encode packets and write to queue
            for (size_t i = 0; i < NumSourcePackets; ++i) {
                writer.write(source_packets[i]);
            }

            // write packets from queue to dispatcher
            for (size_t i = 0; i < NumSourcePackets + NumRepairPackets; ++i) {
                packet::PacketPtr p = queue.read();
                CHECK(p);
                if (i == 5) {
                    // violates: ESI < SBL (for source packets)
                    p->fec()->encoding_symbol_id = NumSourcePackets;
                    recompose_packet(p);
                }
                if (i == NumSourcePackets + 3) {
                    // violates: ESI >= SBL (for repair packets)
                    p->fec()->encoding_symbol_id = NumSourcePackets - 1;
                    recompose_packet(p);
                }
                if (i == NumSourcePackets + 5) {
                    // violates: ESI < NES (for repair packets)
                    p->fec()->encoding_symbol_id = NumSourcePackets + NumRepairPackets;
                    recompose_packet(p);
                }
                dispatcher.write(p);
            }

            // deliver packets from dispatcher to reader
            dispatcher.push_stocks();

            // read packets
            for (size_t i = 0; i < NumSourcePackets; ++i) {
                packet::PacketPtr p = reader.read();
                CHECK(p);
                check_audio_packet(p, i);
                // packet #5 should be dropped and repaired
                check_restored(p, i == 5);
            }

            CHECK(reader.is_alive());
            CHECK(dispatcher.source_size() == 0);
            CHECK(dispatcher.repair_size() == 0);
        }
    }
}

TEST(writer_reader, invalid_sbl) {
    enum { NumBlocks = 5 };

    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        packet::Queue queue;

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, queue,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        for (size_t n_block = 0; n_block < NumBlocks; n_block++) {
            fill_all_packets(0);

            // encode packets and write to queue
            for (size_t i = 0; i < NumSourcePackets; ++i) {
                writer.write(source_packets[i]);
            }

            // write packets from queue to dispatcher
            for (size_t i = 0; i < NumSourcePackets + NumRepairPackets; ++i) {
                packet::PacketPtr p = queue.read();
                CHECK(p);
                if (i == 5) {
                    // violates: SBL can't change in the middle of a block (source packet)
                    p->fec()->source_block_length = NumSourcePackets + 1;
                    recompose_packet(p);
                }
                if (i == NumSourcePackets + 3) {
                    // violates: SBL can't change in the middle of a block (repair packet)
                    p->fec()->source_block_length = NumSourcePackets + 1;
                    recompose_packet(p);
                }
                dispatcher.write(p);
            }

            // deliver packets from dispatcher to reader
            dispatcher.push_stocks();

            // read packets
            for (size_t i = 0; i < NumSourcePackets; ++i) {
                packet::PacketPtr p = reader.read();
                CHECK(p);
                check_audio_packet(p, i);
                // packet #5 should be dropped and repaired
                check_restored(p, i == 5);
            }

            CHECK(reader.is_alive());
            CHECK(dispatcher.source_size() == 0);
            CHECK(dispatcher.repair_size() == 0);
        }
    }
}

TEST(writer_reader, invalid_nes) {
    enum { NumBlocks = 5 };

    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        packet::Queue queue;

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, queue,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        for (size_t n_block = 0; n_block < NumBlocks; n_block++) {
            fill_all_packets(0);

            // encode packets and write to queue
            for (size_t i = 0; i < NumSourcePackets; ++i) {
                writer.write(source_packets[i]);
            }

            // write packets from queue to dispatcher
            for (size_t i = 0; i < NumSourcePackets + NumRepairPackets; ++i) {
                packet::PacketPtr p = queue.read();
                CHECK(p);
                if (i == NumSourcePackets) {
                    // violates: SBL <= NES
                    p->fec()->block_length = NumSourcePackets - 1;
                    recompose_packet(p);
                }
                if (i == NumSourcePackets + 3) {
                    // violates: NES can't change in the middle of a block
                    p->fec()->block_length = NumSourcePackets + NumRepairPackets + 1;
                    recompose_packet(p);
                }
                dispatcher.write(p);
            }

            // deliver packets from dispatcher to reader
            dispatcher.push_stocks();

            // read packets
            for (size_t i = 0; i < NumSourcePackets; ++i) {
                packet::PacketPtr p = reader.read();
                CHECK(p);
                check_audio_packet(p, i);
                check_restored(p, false);
            }

            CHECK(reader.is_alive());
            CHECK(dispatcher.source_size() == 0);
            CHECK(dispatcher.repair_size() == 0);
        }
    }
}

TEST(writer_reader, invalid_payload_size) {
    enum { NumBlocks = 5 };

    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        packet::Queue writer_queue;
        packet::Queue source_queue;
        packet::Queue repair_queue;

        Writer writer(writer_config, codec_config.scheme, *encoder, writer_queue,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder, source_queue,
                      repair_queue, rtp_parser, packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        for (size_t n_block = 0; n_block < NumBlocks; n_block++) {
            fill_all_packets(0);

            // encode packets and write to writer_queue
            for (size_t i = 0; i < NumSourcePackets; ++i) {
                writer.write(source_packets[i]);
            }

            // read packets from writer_queue queue, spoil some packets, and
            // write them to source_queue and repair_queue
            for (size_t i = 0; i < NumSourcePackets + NumRepairPackets; ++i) {
                packet::PacketPtr p = writer_queue.read();
                CHECK(p);

                if (i == 5) {
                    // violates: psize can't change in the middle of a block (source
                    // packet)
                    p->fec()->payload.reslice(0, FECPayloadSize - 1);
                }
                if (i == NumSourcePackets + 3) {
                    // violates: psize can't change in the middle of a block (repair
                    // packet)
                    p->fec()->payload.reslice(0, FECPayloadSize - 1);
                }
                if (n_block == 3 && i == 0) {
                    // violates: psize can't be zero (source packet)
                    p->fec()->payload.reslice(0, 0);
                }
                if (n_block == 4 && i == NumSourcePackets) {
                    // violates: psize can't be zero (repair packet)
                    p->fec()->payload.reslice(0, 0);
                }

                if (p->flags() & packet::Packet::FlagRepair) {
                    repair_queue.write(p);
                } else {
                    source_queue.write(p);
                }
            }

            // read packets
            for (size_t i = 0; i < NumSourcePackets; ++i) {
                packet::PacketPtr p = reader.read();
                CHECK(p);
                check_audio_packet(p, i);
                // invalid packets should be dropped and repaired
                check_restored(p, i == 5 || (n_block == 3 && i == 0));
            }

            CHECK(reader.is_alive());
            CHECK(source_queue.size() == 0);
            CHECK(repair_queue.size() == 0);
        }
    }
}

TEST(writer_reader, zero_source_packets) {
    enum { NumBlocks = 5 };

    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        packet::Queue queue;

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, queue,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        for (size_t n_block = 0; n_block < NumBlocks; n_block++) {
            fill_all_packets(0);

            // encode packets and write to queue
            for (size_t i = 0; i < NumSourcePackets; ++i) {
                writer.write(source_packets[i]);
            }

            // lose source packet #5
            dispatcher.lose(5);

            // write packets from queue to dispatcher
            for (size_t i = 0; i < NumSourcePackets + NumRepairPackets; ++i) {
                packet::PacketPtr p = queue.read();
                CHECK(p);

                // two blocks with SBL == 0
                if (n_block == 2 || n_block == 4) {
                    p->fec()->source_block_length = 0;
                    recompose_packet(p);
                }

                dispatcher.write(p);
            }

            // check we have processed all packets
            UNSIGNED_LONGS_EQUAL(0, queue.size());

            // deliver packets from dispatcher to reader
            dispatcher.push_stocks();

            // read packets
            for (size_t i = 0; i < NumSourcePackets; ++i) {
                packet::PacketPtr p = reader.read();

                if (n_block == 2 || n_block == 4) {
                    CHECK(!p);
                } else {
                    CHECK(p);
                    check_audio_packet(p, i);
                    check_restored(p, i == 5);
                }
            }

            CHECK(reader.is_alive());
            CHECK(dispatcher.source_size() == 0);
            CHECK(dispatcher.repair_size() == 0);
        }
    }
}

TEST(writer_reader, zero_repair_packets) {
    enum { NumBlocks = 5 };

    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        packet::Queue queue;

        test::PacketDispatcher dispatcher(ldpc_source_parser, ldpc_repair_parser,
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, packet::FEC_LDPC_Staircase, *encoder, queue,
                      ldpc_source_composer, ldpc_repair_composer, packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, packet::FEC_LDPC_Staircase, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        for (size_t n_block = 0; n_block < NumBlocks; n_block++) {
            // encode packets and write to queue
            for (size_t i = 0; i < NumSourcePackets; ++i) {
                writer.write(fill_one_packet(i, FECPayloadSize, &ldpc_source_composer));
            }

            // lose source packet #5
            dispatcher.lose(5);

            // write packets from queue to dispatcher
            for (size_t i = 0; i < NumSourcePackets + NumRepairPackets; ++i) {
                packet::PacketPtr p = queue.read();
                CHECK(p);

                // two blocks with NES == SBL
                if ((n_block == 2 || n_block == 4) && (i >= NumSourcePackets)) {
                    p->fec()->block_length = NumSourcePackets;
                    ldpc_repair_composer.compose(*p);
                }

                dispatcher.write(p);
            }

            // check we have processed all packets
            UNSIGNED_LONGS_EQUAL(0, queue.size());

            // deliver packets from dispatcher to reader
            dispatcher.push_stocks();

            // read packets
            for (size_t i = 0; i < NumSourcePackets; ++i) {
                if ((n_block == 2 || n_block == 4) && (i == 5)) {
                    // nop
                } else {
                    packet::PacketPtr p = reader.read();
                    CHECK(p);
                    check_audio_packet(p, i);
                    check_restored(p, i == 5);
                }
            }

            CHECK(reader.is_alive());
            CHECK(dispatcher.source_size() == 0);
            CHECK(dispatcher.repair_size() == 0);
        }
    }
}

TEST(writer_reader, zero_payload_size) {
    enum { NumBlocks = 5 };

    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        packet::Queue writer_queue;
        packet::Queue source_queue;
        packet::Queue repair_queue;

        Writer writer(writer_config, codec_config.scheme, *encoder, writer_queue,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder, source_queue,
                      repair_queue, rtp_parser, packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        for (size_t n_block = 0; n_block < NumBlocks; n_block++) {
            fill_all_packets(0);

            // encode packets and write to writer_queue
            for (size_t i = 0; i < NumSourcePackets; ++i) {
                writer.write(source_packets[i]);
            }

            // read packets from writer_queue queue, spoil some packets, and
            // write them to source_queue and repair_queue
            for (size_t i = 0; i < NumSourcePackets + NumRepairPackets; ++i) {
                packet::PacketPtr p = writer_queue.read();
                CHECK(p);

                // loss packet #5
                if (i == 5) {
                    continue;
                }

                // two blocks with invalid zero-payload packets
                if (n_block == 2 || n_block == 4) {
                    p->fec()->payload.reslice(0, 0);
                }

                if (p->flags() & packet::Packet::FlagRepair) {
                    repair_queue.write(p);
                } else {
                    source_queue.write(p);
                }
            }

            // check we have processed all packets
            UNSIGNED_LONGS_EQUAL(0, writer_queue.size());

            // read packets
            for (size_t i = 0; i < NumSourcePackets; ++i) {
                packet::PacketPtr p = reader.read();

                if (n_block == 2 || n_block == 4) {
                    CHECK(!p);
                } else {
                    CHECK(p);
                    check_audio_packet(p, i);
                    check_restored(p, i == 5);
                }
            }

            CHECK(reader.is_alive());
            CHECK(source_queue.size() == 0);
            CHECK(repair_queue.size() == 0);
        }
    }
}

TEST(writer_reader, sbn_jump) {
    enum { MaxSbnJump = 30 };

    reader_config.max_sbn_jump = MaxSbnJump;

    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        packet::Queue queue;

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, queue,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        // write three blocks to the queue
        for (size_t n = 0; n < 3; n++) {
            fill_all_packets(NumSourcePackets * n);

            for (size_t i = 0; i < NumSourcePackets; ++i) {
                writer.write(source_packets[i]);
            }
        }

        // write first block to the dispatcher
        for (size_t i = 0; i < NumSourcePackets + NumRepairPackets; ++i) {
            packet::PacketPtr p = queue.read();
            CHECK(p);
            dispatcher.write(p);
        }

        // deliver packets from dispatcher to reader
        dispatcher.push_stocks();

        // read first block
        for (size_t i = 0; i < NumSourcePackets; ++i) {
            packet::PacketPtr p = reader.read();
            CHECK(p);
            check_audio_packet(p, i);
            check_restored(p, false);
        }

        CHECK(reader.is_alive());

        // write second block to the dispatcher
        // shift it ahead but in the allowed range
        for (size_t i = 0; i < NumSourcePackets + NumRepairPackets; ++i) {
            packet::PacketPtr p = queue.read();
            CHECK(p);

            p->fec()->source_block_number += MaxSbnJump;
            recompose_packet(p);

            dispatcher.write(p);
        }

        // deliver packets from dispatcher to reader
        dispatcher.push_stocks();

        // read second block
        for (size_t i = NumSourcePackets; i < NumSourcePackets * 2; ++i) {
            packet::PacketPtr p = reader.read();
            CHECK(p);
            check_audio_packet(p, i);
            check_restored(p, false);
        }

        CHECK(reader.is_alive());

        // write third block to the dispatcher
        // shift it ahead too far
        for (size_t i = 0; i < NumSourcePackets + NumRepairPackets; ++i) {
            packet::PacketPtr p = queue.read();
            CHECK(p);

            p->fec()->source_block_number += MaxSbnJump * 2 + 1;
            recompose_packet(p);

            dispatcher.write(p);
        }

        // deliver packets from dispatcher to reader
        dispatcher.push_stocks();

        // the reader should detect sbn jump and shutdown
        CHECK(!reader.read());
        CHECK(!reader.is_alive());

        CHECK(dispatcher.source_size() == 0);
        CHECK(dispatcher.repair_size() == 0);
    }
}

TEST(writer_reader, writer_encode_blocks) {
    enum { NumBlocks = 3 };

    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        packet::source_t data_source = 555;

        for (size_t n = 0; n < 5; n++) {
            core::ScopedPtr<IBlockEncoder> encoder(
                CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
                allocator);

            CHECK(encoder);

            test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                              packet_factory, NumSourcePackets,
                                              NumRepairPackets);

            Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                          source_composer(), repair_composer(), packet_factory,
                          buffer_factory, allocator);

            CHECK(writer.is_valid());

            packet::blknum_t fec_sbn = 0;

            for (size_t block_num = 0; block_num < NumBlocks; ++block_num) {
                size_t encoding_symbol_id = 0;

                fill_all_packets(NumSourcePackets * block_num);

                for (size_t i = 0; i < NumSourcePackets; ++i) {
                    source_packets[i]->rtp()->source = data_source;
                }

                for (size_t i = 0; i < NumSourcePackets; ++i) {
                    writer.write(source_packets[i]);
                }
                dispatcher.push_stocks();

                if (block_num == 0) {
                    const packet::FEC* fec = dispatcher.repair_head()->fec();
                    CHECK(fec);

                    fec_sbn = fec->source_block_number;
                }

                for (size_t i = 0; i < NumSourcePackets; ++i) {
                    const packet::PacketPtr p = dispatcher.source_reader().read();
                    CHECK(p);

                    const packet::RTP* rtp = p->rtp();
                    CHECK(rtp);

                    LONGS_EQUAL(data_source, rtp->source);

                    const packet::FEC* fec = p->fec();
                    CHECK(fec);

                    LONGS_EQUAL(fec_sbn, fec->source_block_number);
                    CHECK(fec->source_block_length == NumSourcePackets);
                    UNSIGNED_LONGS_EQUAL(encoding_symbol_id, fec->encoding_symbol_id);

                    encoding_symbol_id++;
                }

                for (size_t i = 0; i < NumRepairPackets; ++i) {
                    const packet::PacketPtr p = dispatcher.repair_reader().read();
                    CHECK(p);

                    const packet::RTP* rtp = p->rtp();
                    CHECK(!rtp);

                    const packet::FEC* fec = p->fec();
                    CHECK(fec);

                    LONGS_EQUAL(fec_sbn, fec->source_block_number);
                    CHECK(fec->source_block_length == NumSourcePackets);
                    UNSIGNED_LONGS_EQUAL(encoding_symbol_id, fec->encoding_symbol_id);

                    encoding_symbol_id++;
                }

                fec_sbn++;
            }

            dispatcher.reset();
        }
    }
}

TEST(writer_reader, writer_resize_blocks) {
    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);
        CHECK(encoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        CHECK(writer.is_valid());

        const size_t source_sizes[] = {
            15, 25, 35, 43, 33, 23, 13, 255 - NumRepairPackets
        };

        const size_t repair_sizes[] = { 10, 20, 30, 40, 30, 20, 10, NumRepairPackets };

        const size_t payload_sizes[] = { 100, 100, 100, 80, 150, 170, 170, 90 };

        UNSIGNED_LONGS_EQUAL(ROC_ARRAY_SIZE(source_sizes), ROC_ARRAY_SIZE(repair_sizes));
        UNSIGNED_LONGS_EQUAL(ROC_ARRAY_SIZE(source_sizes), ROC_ARRAY_SIZE(payload_sizes));

        packet::seqnum_t wr_sn = 0;
        packet::seqnum_t rd_sn = 0;

        for (size_t n = 0; n < ROC_ARRAY_SIZE(source_sizes); ++n) {
            CHECK(writer.resize(source_sizes[n], repair_sizes[n]));

            for (size_t i = 0; i < source_sizes[n]; ++i) {
                packet::PacketPtr p = fill_one_packet(wr_sn, payload_sizes[n]);
                wr_sn++;
                writer.write(p);
            }

            UNSIGNED_LONGS_EQUAL(source_sizes[n], dispatcher.source_size());
            UNSIGNED_LONGS_EQUAL(repair_sizes[n], dispatcher.repair_size());

            dispatcher.push_stocks();

            for (size_t i = 0; i < source_sizes[n]; ++i) {
                packet::PacketPtr p = dispatcher.source_reader().read();
                CHECK(p);
                check_audio_packet(p, rd_sn, payload_sizes[n]);
                rd_sn++;
            }

            dispatcher.reset();
        }

        UNSIGNED_LONGS_EQUAL(wr_sn, rd_sn);
    }
}

TEST(writer_reader, resize_block_begin) {
    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(decoder);
        CHECK(encoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);
        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(reader.is_valid());
        CHECK(writer.is_valid());

        const size_t source_sizes[] = {
            15, 25, 35, 43, 33, 23, 13, 255 - NumRepairPackets
        };

        const size_t repair_sizes[] = { 10, 20, 30, 40, 30, 20, 10, NumRepairPackets };

        const size_t payload_sizes[] = { 100, 100, 100, 80, 150, 170, 170, 90 };

        UNSIGNED_LONGS_EQUAL(ROC_ARRAY_SIZE(source_sizes), ROC_ARRAY_SIZE(repair_sizes));
        UNSIGNED_LONGS_EQUAL(ROC_ARRAY_SIZE(source_sizes), ROC_ARRAY_SIZE(payload_sizes));

        packet::seqnum_t wr_sn = 0;
        packet::seqnum_t rd_sn = 0;

        for (size_t n = 0; n < ROC_ARRAY_SIZE(source_sizes); ++n) {
            CHECK(writer.resize(source_sizes[n], repair_sizes[n]));

            for (size_t i = 0; i < source_sizes[n]; ++i) {
                packet::PacketPtr p = fill_one_packet(wr_sn, payload_sizes[n]);
                wr_sn++;
                writer.write(p);
            }

            UNSIGNED_LONGS_EQUAL(source_sizes[n], dispatcher.source_size());
            UNSIGNED_LONGS_EQUAL(repair_sizes[n], dispatcher.repair_size());

            dispatcher.push_stocks();

            for (size_t i = 0; i < source_sizes[n]; ++i) {
                const packet::PacketPtr p = reader.read();

                CHECK(p);
                CHECK(p->fec());
                UNSIGNED_LONGS_EQUAL(source_sizes[n], p->fec()->source_block_length);

                check_audio_packet(p, rd_sn, payload_sizes[n]);
                check_restored(p, false);

                rd_sn++;
            }
        }

        UNSIGNED_LONGS_EQUAL(wr_sn, rd_sn);
    }
}

TEST(writer_reader, resize_block_middle) {
    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(decoder);
        CHECK(encoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);
        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(reader.is_valid());
        CHECK(writer.is_valid());

        const size_t source_sizes[] = {
            15, 25, 35, 43, 33, 23, 13, 255 - NumRepairPackets
        };

        const size_t repair_sizes[] = { 10, 20, 30, 40, 30, 20, 10, NumRepairPackets };

        const size_t payload_sizes[] = { 100, 100, 100, 80, 150, 170, 170, 90 };

        UNSIGNED_LONGS_EQUAL(ROC_ARRAY_SIZE(source_sizes), ROC_ARRAY_SIZE(repair_sizes));
        UNSIGNED_LONGS_EQUAL(ROC_ARRAY_SIZE(source_sizes), ROC_ARRAY_SIZE(payload_sizes));

        size_t prev_sblen = NumSourcePackets;
        size_t prev_rblen = NumRepairPackets;
        size_t prev_psize = FECPayloadSize;

        packet::seqnum_t wr_sn = 0;
        packet::seqnum_t rd_sn = 0;

        for (size_t n = 0; n < ROC_ARRAY_SIZE(source_sizes); ++n) {
            core::Array<packet::PacketPtr> packets(allocator);
            packets.resize(prev_sblen);

            for (size_t i = 0; i < prev_sblen; ++i) {
                packets[i] = fill_one_packet(wr_sn, prev_psize);
                wr_sn++;
            }

            // Write first half of the packets.
            for (size_t i = 0; i < prev_sblen / 2; ++i) {
                writer.write(packets[i]);
            }

            // Update source block size.
            CHECK(writer.resize(source_sizes[n], repair_sizes[n]));

            // Write the remaining packets.
            for (size_t i = prev_sblen / 2; i < prev_sblen; ++i) {
                writer.write(packets[i]);
            }

            UNSIGNED_LONGS_EQUAL(prev_sblen, dispatcher.source_size());
            UNSIGNED_LONGS_EQUAL(prev_rblen, dispatcher.repair_size());

            dispatcher.push_stocks();

            for (size_t i = 0; i < prev_sblen; ++i) {
                const packet::PacketPtr p = reader.read();

                CHECK(p);
                CHECK(p->fec());
                CHECK(p->fec()->source_block_length == prev_sblen);

                check_audio_packet(p, rd_sn, prev_psize);
                check_restored(p, false);

                rd_sn++;
            }

            prev_sblen = source_sizes[n];
            prev_rblen = repair_sizes[n];
            prev_psize = payload_sizes[n];
        }

        UNSIGNED_LONGS_EQUAL(wr_sn, rd_sn);
    }
}

TEST(writer_reader, resize_block_losses) {
    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(decoder);
        CHECK(encoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);
        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(reader.is_valid());
        CHECK(writer.is_valid());

        const size_t source_sizes[] = {
            15, 25, 35, 43, 33, 23, 13, 255 - NumRepairPackets
        };

        const size_t repair_sizes[] = { 10, 20, 30, 40, 30, 20, 10, NumRepairPackets };

        const size_t payload_sizes[] = { 100, 100, 100, 80, 150, 170, 170, 90 };

        UNSIGNED_LONGS_EQUAL(ROC_ARRAY_SIZE(source_sizes), ROC_ARRAY_SIZE(repair_sizes));
        UNSIGNED_LONGS_EQUAL(ROC_ARRAY_SIZE(source_sizes), ROC_ARRAY_SIZE(payload_sizes));

        packet::seqnum_t wr_sn = 0;
        packet::seqnum_t rd_sn = 0;

        for (size_t n = 0; n < ROC_ARRAY_SIZE(source_sizes); ++n) {
            CHECK(writer.resize(source_sizes[n], repair_sizes[n]));

            dispatcher.resize(source_sizes[n], repair_sizes[n]);
            dispatcher.reset();

            dispatcher.lose(source_sizes[n] / 2);

            for (size_t i = 0; i < source_sizes[n]; ++i) {
                packet::PacketPtr p = fill_one_packet(wr_sn, payload_sizes[n]);
                wr_sn++;
                writer.write(p);
            }

            UNSIGNED_LONGS_EQUAL(source_sizes[n] - 1, dispatcher.source_size());
            UNSIGNED_LONGS_EQUAL(repair_sizes[n], dispatcher.repair_size());

            dispatcher.push_stocks();

            for (size_t i = 0; i < source_sizes[n]; ++i) {
                const packet::PacketPtr p = reader.read();
                CHECK(p);

                check_audio_packet(p, rd_sn, payload_sizes[n]);
                check_restored(p, i == source_sizes[n] / 2);

                rd_sn++;
            }
        }

        UNSIGNED_LONGS_EQUAL(wr_sn, rd_sn);
    }
}

TEST(writer_reader, resize_block_repair_first) {
    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        packet::seqnum_t wr_sn = 0;
        packet::seqnum_t rd_sn = 0;

        // Encode first block.
        for (size_t i = 0; i < NumSourcePackets; ++i) {
            writer.write(fill_one_packet(wr_sn));
            wr_sn++;
        }

        // Deliver first block.
        dispatcher.push_stocks();

        // Read first block.
        for (size_t i = 0; i < NumSourcePackets; ++i) {
            packet::PacketPtr p = reader.read();
            CHECK(p);
            check_audio_packet(p, rd_sn);
            check_restored(p, false);
            rd_sn++;
        }

        // Resize.
        CHECK(writer.resize(NumSourcePackets * 2, NumRepairPackets * 2));

        // Lose one packet.
        dispatcher.resize(NumSourcePackets * 2, NumRepairPackets * 2);
        dispatcher.lose(NumSourcePackets + 3);

        // Encode second block.
        for (size_t i = 0; i < NumSourcePackets * 2; ++i) {
            writer.write(fill_one_packet(wr_sn, FECPayloadSize * 2));
            wr_sn++;
        }

        // Deliver repair packets from second block.
        dispatcher.push_repair_stock(NumRepairPackets * 2);

        // Try and fail to read first packet from second block.
        CHECK(!reader.read());

        // Deliver source packets from second block.
        dispatcher.push_source_stock(NumSourcePackets * 2 - 1);

        // Read second block.
        for (size_t i = 0; i < NumSourcePackets * 2; ++i) {
            packet::PacketPtr p = reader.read();
            CHECK(p);
            check_audio_packet(p, rd_sn, FECPayloadSize * 2);
            check_restored(p, i == NumSourcePackets + 3);
            rd_sn++;
        }

        CHECK(dispatcher.source_size() == 0);
        CHECK(dispatcher.repair_size() == 0);

        UNSIGNED_LONGS_EQUAL(wr_sn, rd_sn);
    }
}

TEST(writer_reader, error_writer_resize_block) {
    enum { BlockSize1 = 50, BlockSize2 = 60 };

    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);
        CHECK(encoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        test::MockAllocator mock_allocator;

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, mock_allocator);

        CHECK(writer.is_valid());

        size_t sn = 0;

        CHECK(writer.resize(NumSourcePackets, BlockSize1));

        for (size_t i = 0; i < NumSourcePackets; ++i) {
            writer.write(fill_one_packet(sn++));
        }

        CHECK(writer.is_alive());
        CHECK(dispatcher.source_size() == NumSourcePackets);
        CHECK(dispatcher.repair_size() == BlockSize1);

        dispatcher.push_stocks();
        dispatcher.reset();

        mock_allocator.set_fail(true);

        CHECK(writer.resize(NumSourcePackets, BlockSize2));

        for (size_t i = 0; i < NumSourcePackets; ++i) {
            writer.write(fill_one_packet(sn++));
            CHECK(!writer.is_alive());
        }

        CHECK(dispatcher.source_size() == 0);
        CHECK(dispatcher.repair_size() == 0);
    }
}

TEST(writer_reader, error_writer_encode_packet) {
    enum { BlockSize1 = 50, BlockSize2 = 60 };

    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        test::MockAllocator mock_allocator;

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory,
                                             mock_allocator),
            mock_allocator);
        CHECK(encoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        CHECK(writer.is_valid());

        size_t sn = 0;

        CHECK(writer.resize(BlockSize1, NumRepairPackets));

        for (size_t i = 0; i < BlockSize1; ++i) {
            writer.write(fill_one_packet(sn++));
        }

        CHECK(writer.is_alive());
        CHECK(dispatcher.source_size() == BlockSize1);
        CHECK(dispatcher.repair_size() == NumRepairPackets);

        mock_allocator.set_fail(true);
        CHECK(writer.resize(BlockSize2, NumRepairPackets));

        for (size_t i = 0; i < BlockSize2; ++i) {
            writer.write(fill_one_packet(sn++));
        }

        CHECK(!writer.is_alive());
        CHECK(dispatcher.source_size() == BlockSize1);
        CHECK(dispatcher.repair_size() == NumRepairPackets);
    }
}

TEST(writer_reader, error_reader_resize_block) {
    enum { BlockSize1 = 50, BlockSize2 = 60 };

    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        test::MockAllocator mock_allocator;

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, mock_allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        size_t sn = 0;

        // write first block
        CHECK(writer.resize(BlockSize1, NumRepairPackets));
        for (size_t i = 0; i < BlockSize1; ++i) {
            writer.write(fill_one_packet(sn++));
        }

        // deliver first block
        dispatcher.push_stocks();

        // read first block
        for (size_t i = 0; i < BlockSize1; ++i) {
            packet::PacketPtr p = reader.read();
            CHECK(p);
            check_audio_packet(p, i);
            check_restored(p, false);
        }

        // write second block
        CHECK(writer.resize(BlockSize2, NumRepairPackets));
        for (size_t i = 0; i < BlockSize2; ++i) {
            writer.write(fill_one_packet(sn++));
        }

        // deliver second block
        dispatcher.push_stocks();

        // configure allocator to return errors
        mock_allocator.set_fail(true);

        // reader should get an error from allocator when trying
        // to resize the block and shut down
        CHECK(!reader.read());
        CHECK(!reader.is_alive());
    }
}

TEST(writer_reader, error_reader_decode_packet) {
    enum { BlockSize1 = 50, BlockSize2 = 60 };

    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); n_scheme++) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);
        CHECK(encoder);

        test::MockAllocator mock_allocator;

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory,
                                             mock_allocator),
            mock_allocator);
        CHECK(decoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        size_t sn = 0;

        // write first block
        CHECK(writer.resize(BlockSize1, NumRepairPackets));
        for (size_t i = 0; i < BlockSize1; ++i) {
            writer.write(fill_one_packet(sn++));
        }

        // deliver first block
        dispatcher.push_stocks();

        // read first block
        for (size_t i = 0; i < BlockSize1; ++i) {
            packet::PacketPtr p = reader.read();
            CHECK(p);
            check_audio_packet(p, i);
            check_restored(p, false);
        }

        // lose one packet in second block
        dispatcher.reset();
        dispatcher.lose(10);

        // write second block
        CHECK(writer.resize(BlockSize2, NumRepairPackets));
        for (size_t i = 0; i < BlockSize2; ++i) {
            writer.write(fill_one_packet(sn++));
        }

        // deliver second block
        dispatcher.push_stocks();

        // read second block packets before loss
        for (size_t i = 0; i < 10; ++i) {
            packet::PacketPtr p = reader.read();
            CHECK(p);
            check_audio_packet(p, BlockSize1 + i);
            check_restored(p, false);
        }

        // configure allocator to return errors
        mock_allocator.set_fail(true);

        // reader should get an error from allocator when trying
        // to repair lost packet and shut down
        CHECK(!reader.read());
        CHECK(!reader.is_alive());
    }
}

TEST(writer_reader, writer_oversized_block) {
    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); ++n_scheme) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        CHECK(decoder->max_block_length() == encoder->max_block_length());
        CHECK(NumSourcePackets + NumRepairPackets <= encoder->max_block_length());

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        // try to resize writer with an invalid value
        CHECK(!writer.resize(encoder->max_block_length() + 1, NumRepairPackets));

        // ensure that the block size was not updated
        for (size_t n = 0; n < 10; ++n) {
            fill_all_packets(0);

            // write packets to dispatcher
            for (size_t i = 0; i < NumSourcePackets; ++i) {
                writer.write(source_packets[i]);
            }

            // deliver packets from dispatcher to reader
            dispatcher.push_stocks();

            CHECK(dispatcher.source_size() == NumSourcePackets);
            CHECK(dispatcher.repair_size() == NumRepairPackets);

            // read packets
            for (size_t i = 0; i < NumSourcePackets; ++i) {
                packet::PacketPtr p = reader.read();
                CHECK(p);

                check_audio_packet(p, i);
                check_restored(p, false);

                UNSIGNED_LONGS_EQUAL(NumSourcePackets, p->fec()->source_block_length);
            }

            CHECK(reader.is_alive());
            CHECK(dispatcher.source_size() == 0);
            CHECK(dispatcher.repair_size() == 0);
        }
    }
}

TEST(writer_reader, reader_oversized_source_block) {
    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); ++n_scheme) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        CHECK(decoder->max_block_length() == encoder->max_block_length());
        CHECK((NumSourcePackets + NumRepairPackets) < encoder->max_block_length());

        packet::Queue queue;
        test::PacketDispatcher dispatcher(ldpc_source_parser, ldpc_repair_parser,
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        // We are going to spoil source_block_length field of a FEC packet,
        // but Reed-Solomon does not allow us to set this field above 255,
        // so LDPC composer is used for all schemes.
        Writer writer(writer_config, packet::FEC_LDPC_Staircase, *encoder, queue,
                      ldpc_source_composer, ldpc_repair_composer, packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, packet::FEC_LDPC_Staircase, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        // encode packets and write to queue
        for (size_t i = 0; i < NumSourcePackets; ++i) {
            writer.write(fill_one_packet(i, FECPayloadSize, &ldpc_source_composer));
        }

        // write packets from queue to dispatcher
        for (size_t i = 0; i < NumSourcePackets + NumRepairPackets; ++i) {
            packet::PacketPtr p = queue.read();
            CHECK(p);

            // update block size at the beginning of the block
            if (i == 0) {
                // violates: SBL <= MAX_BLEN (for source packets)
                p->fec()->source_block_length = encoder->max_block_length() + 1;
                ldpc_source_composer.compose(*p);
            }

            dispatcher.write(p);
        }

        // deliver packets from dispatcher to reader
        dispatcher.push_stocks();

        CHECK(dispatcher.source_size() == NumSourcePackets);
        CHECK(dispatcher.repair_size() == NumRepairPackets);

        // reader should get an error because maximum block size was exceeded
        CHECK(!reader.read());
        CHECK(!reader.is_alive());
    }
}

TEST(writer_reader, reader_oversized_repair_block) {
    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); ++n_scheme) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        CHECK(decoder->max_block_length() == encoder->max_block_length());
        CHECK((NumSourcePackets + NumRepairPackets) < encoder->max_block_length());

        packet::Queue queue;
        test::PacketDispatcher dispatcher(ldpc_source_parser, ldpc_repair_parser,
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        // We are going to spoil source_block_length field of a FEC packet,
        // but Reed-Solomon does not allow us to set this field above 255,
        // so LDPC composer is used for all schemes.
        Writer writer(writer_config, packet::FEC_LDPC_Staircase, *encoder, queue,
                      ldpc_source_composer, ldpc_repair_composer, packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, packet::FEC_LDPC_Staircase, *decoder,
                      dispatcher.source_reader(), dispatcher.repair_reader(), rtp_parser,
                      packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        // encode packets and write to queue
        for (size_t i = 0; i < NumSourcePackets; ++i) {
            writer.write(fill_one_packet(i, FECPayloadSize, &ldpc_source_composer));
        }

        // write packets from queue to dispatcher
        for (size_t i = 0; i < NumSourcePackets + NumRepairPackets; ++i) {
            packet::PacketPtr p = queue.read();
            CHECK(p);

            // update block size at the beginning of the block
            if (i == NumSourcePackets) {
                // violates: BLEN <= MAX_BLEN (for repair packets)
                p->fec()->block_length = encoder->max_block_length() + 1;
                ldpc_repair_composer.compose(*p);
            }

            dispatcher.write(p);
        }

        // deliver packets from dispatcher to reader
        dispatcher.push_stocks();

        CHECK(dispatcher.source_size() == NumSourcePackets);
        CHECK(dispatcher.repair_size() == NumRepairPackets);

        // reader should get an error because maximum block size was exceeded
        CHECK(!reader.read());
        CHECK(!reader.is_alive());
    }
}

TEST(writer_reader, writer_invalid_payload_size_change) {
    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); ++n_scheme) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);
        CHECK(encoder);

        test::PacketDispatcher dispatcher(source_parser(), repair_parser(),
                                          packet_factory, NumSourcePackets,
                                          NumRepairPackets);

        Writer writer(writer_config, codec_config.scheme, *encoder, dispatcher,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);
        CHECK(writer.is_valid());

        size_t sn = 0;

        // write the first block with the same payload size
        for (size_t i = 0; i < NumSourcePackets; ++i) {
            writer.write(fill_one_packet(sn++, FECPayloadSize));
        }

        CHECK(writer.is_alive());
        UNSIGNED_LONGS_EQUAL(NumSourcePackets, dispatcher.source_size());
        UNSIGNED_LONGS_EQUAL(NumRepairPackets, dispatcher.repair_size());

        // write a half of the second block with another payload size
        for (size_t i = 0; i < NumSourcePackets / 2; ++i) {
            writer.write(fill_one_packet(sn++, FECPayloadSize - 1));
        }

        CHECK(writer.is_alive());
        UNSIGNED_LONGS_EQUAL(NumSourcePackets + NumSourcePackets / 2,
                             dispatcher.source_size());
        UNSIGNED_LONGS_EQUAL(NumRepairPackets, dispatcher.repair_size());

        // write a packet with different payload size
        writer.write(fill_one_packet(sn, FECPayloadSize));

        // writer should be terminated
        CHECK(!writer.is_alive());
        UNSIGNED_LONGS_EQUAL(NumSourcePackets + NumSourcePackets / 2,
                             dispatcher.source_size());
        UNSIGNED_LONGS_EQUAL(NumRepairPackets, dispatcher.repair_size());
    }
}

TEST(writer_reader, reader_invalid_fec_scheme_source_packet) {
    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); ++n_scheme) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        packet::Queue writer_queue;
        packet::Queue source_queue;
        packet::Queue repair_queue;

        Writer writer(writer_config, codec_config.scheme, *encoder, writer_queue,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder, source_queue,
                      repair_queue, rtp_parser, packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        // encode packets and write to queue
        for (size_t i = 0; i < NumSourcePackets; ++i) {
            writer.write(fill_one_packet(i));
        }
        UNSIGNED_LONGS_EQUAL(NumSourcePackets + NumRepairPackets, writer_queue.size());

        // deliver some of these packets
        for (size_t i = 0; i < NumSourcePackets / 2; ++i) {
            packet::PacketPtr p = writer_queue.read();
            CHECK(p);
            CHECK((p->flags() & packet::Packet::FlagRepair) == 0);
            source_queue.write(p);
        }
        UNSIGNED_LONGS_EQUAL(NumSourcePackets / 2, source_queue.size());

        // read delivered packets
        for (size_t i = 0; i < NumSourcePackets / 2; ++i) {
            CHECK(reader.read());
            CHECK(reader.is_alive());
        }
        UNSIGNED_LONGS_EQUAL(0, source_queue.size());

        // deliver one more source packet but with spoiled fec scheme
        {
            packet::PacketPtr p = writer_queue.read();
            CHECK(p);
            CHECK((p->flags() & packet::Packet::FlagRepair) == 0);
            p->fec()->fec_scheme = CodecMap::instance().nth_scheme(
                (n_scheme + 1) % CodecMap::instance().num_schemes());
            source_queue.write(p);
            UNSIGNED_LONGS_EQUAL(1, source_queue.size());
        }

        // reader should shut down
        CHECK(!reader.read());
        CHECK(!reader.is_alive());
        UNSIGNED_LONGS_EQUAL(0, source_queue.size());
    }
}

TEST(writer_reader, reader_invalid_fec_scheme_repair_packet) {
    for (size_t n_scheme = 0; n_scheme < CodecMap::instance().num_schemes(); ++n_scheme) {
        codec_config.scheme = CodecMap::instance().nth_scheme(n_scheme);

        core::ScopedPtr<IBlockEncoder> encoder(
            CodecMap::instance().new_encoder(codec_config, buffer_factory, allocator),
            allocator);

        core::ScopedPtr<IBlockDecoder> decoder(
            CodecMap::instance().new_decoder(codec_config, buffer_factory, allocator),
            allocator);

        CHECK(encoder);
        CHECK(decoder);

        packet::Queue writer_queue;
        packet::Queue source_queue;
        packet::Queue repair_queue;

        Writer writer(writer_config, codec_config.scheme, *encoder, writer_queue,
                      source_composer(), repair_composer(), packet_factory,
                      buffer_factory, allocator);

        Reader reader(reader_config, codec_config.scheme, *decoder, source_queue,
                      repair_queue, rtp_parser, packet_factory, allocator);

        CHECK(writer.is_valid());
        CHECK(reader.is_valid());

        // encode packets and write to queue
        for (size_t i = 0; i < NumSourcePackets * 2; ++i) {
            writer.write(fill_one_packet(i));
        }
        UNSIGNED_LONGS_EQUAL((NumSourcePackets + NumRepairPackets) * 2,
                             writer_queue.size());

        // deliver some of the source packets
        for (size_t i = 0; i < NumSourcePackets; ++i) {
            packet::PacketPtr p = writer_queue.read();
            CHECK(p);
            CHECK((p->flags() & packet::Packet::FlagRepair) == 0);
            source_queue.write(p);
        }
        UNSIGNED_LONGS_EQUAL(NumSourcePackets, source_queue.size());

        // deliver some of the repair packets
        for (size_t i = 0; i < NumRepairPackets / 2; ++i) {
            packet::PacketPtr p = writer_queue.read();
            CHECK(p);
            CHECK((p->flags() & packet::Packet::FlagRepair) != 0);
            repair_queue.write(p);
        }
        UNSIGNED_LONGS_EQUAL(NumRepairPackets / 2, repair_queue.size());

        // read delivered packets
        for (size_t i = 0; i < NumSourcePackets / 2; ++i) {
            CHECK(reader.read());
            CHECK(reader.is_alive());
        }
        UNSIGNED_LONGS_EQUAL(0, source_queue.size());
        UNSIGNED_LONGS_EQUAL(0, repair_queue.size());

        // deliver one repair packet but with spoiled fec scheme
        {
            packet::PacketPtr p = writer_queue.read();
            CHECK(p);
            CHECK((p->flags() & packet::Packet::FlagRepair) != 0);
            p->fec()->fec_scheme = CodecMap::instance().nth_scheme(
                (n_scheme + 1) % CodecMap::instance().num_schemes());
            repair_queue.write(p);
            UNSIGNED_LONGS_EQUAL(1, repair_queue.size());
        }

        // drop other repair packets
        for (size_t i = 0; i < NumRepairPackets - NumRepairPackets / 2 - 1; ++i) {
            packet::PacketPtr p = writer_queue.read();
            CHECK(p);
            CHECK((p->flags() & packet::Packet::FlagRepair) != 0);
        }

        // deliver more source packets
        for (size_t i = 0; i < NumSourcePackets; ++i) {
            packet::PacketPtr p = writer_queue.read();
            CHECK(p);
            CHECK((p->flags() & packet::Packet::FlagRepair) == 0);
            source_queue.write(p);
        }
        UNSIGNED_LONGS_EQUAL(NumSourcePackets, source_queue.size());

        // reader should shut down
        CHECK(!reader.read());
        CHECK(!reader.is_alive());
        UNSIGNED_LONGS_EQUAL(0, source_queue.size());
        UNSIGNED_LONGS_EQUAL(0, repair_queue.size());
    }
}

} // namespace fec
} // namespace roc
