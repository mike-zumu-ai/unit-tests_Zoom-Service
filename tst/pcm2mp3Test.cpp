//
// ZoomAI service Unit Tests
//

#include "../lib/catch2/catch.hh"

#define private public
#include "pcm2mp3.h"
#undef private

TEST_CASE( "pcm2mp3", "start() method tests" ) {
    pcm2mp3 mp3Convertor;

    void (*callback)(const std::vector<uit8_t>);
    mp3Convertor.start(callback);
    REQUIRE(mp3Convertor.format_.size() != 0);

    mp3Convertor.stop();
    REQUIRE(mp3Convertor.format_.size() == 0);
}
