#include "AuEncoder.h"

#include "gtest/gtest.h"
#include <iostream>
#include <sstream>
#include <string>

TEST(Au, creation) {
    std::ostringstream os;
    Au au(os);
}

TEST(AuStringIntern, NoIntern) {
    AuStringIntern si;
    EXPECT_EQ(0, si.dict().size());
    EXPECT_EQ(-1, si.idx(std::string("shrt")));
    EXPECT_EQ(-1, si.idx(std::string("Long string")));
    EXPECT_EQ(0, si.dict().size());
}

TEST(AuStringIntern, ForceIntern) {
    AuStringIntern si;
    EXPECT_EQ(0, si.dict().size());

    // Tiny strings are not interned even if forced
    EXPECT_LT(si.idx(std::string("tiny"), true), 0);
    EXPECT_EQ(0, si.dict().size());

    EXPECT_EQ(0, si.idx(std::string("A normal string"), true));
    EXPECT_EQ(1, si.dict().size());
}

TEST(AuStringIntern, InternFrequentStrings) {
    AuStringIntern si;
    std::string str("Normal value");

    EXPECT_LT(si.idx(str), 0);
    EXPECT_EQ(0, si.dict().size());

    for (size_t i = 0; i < AuStringIntern::INTERN_THRESH * 2; ++i) {
        if (i < AuStringIntern::INTERN_THRESH) {
            EXPECT_LE(si.idx(str), 0) << i;
            //EXPECT_EQ(0, si.dict().size()) << i;
        } else {
            EXPECT_GE(si.idx(str), 0);
            EXPECT_EQ(1, si.dict().size());
        }
    }
}

//TEST(AuStringIntern, 
