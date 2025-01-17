//------------------------------------------------------------------------------
/*
    This file is part of clio: https://github.com/XRPLF/clio
    Copyright (c) 2022, the clio developers.

    Permission to use, copy, modify, and distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL,  DIRECT,  INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#pragma once

#include <ios>
#include <mutex>
#include <thread>

#include <boost/asio.hpp>

#include <gtest/gtest.h>
#include <log/Logger.h>

/**
 * @brief Fixture with LogService support.
 */
class LoggerFixture : public ::testing::Test
{
    /**
     * @brief A simple string buffer that can be used to mock std::cout for
     * console logging.
     */
    class FakeBuffer final : public std::stringbuf
    {
    public:
        std::string
        getStrAndReset()
        {
            auto value = str();
            str("");
            return value;
        }
    };

    FakeBuffer buffer_;
    std::ostream stream_ = std::ostream{&buffer_};

protected:
    // Simulates the `LogService::init(config)` call
    void
    SetUp() override
    {
        static std::once_flag once_;
        std::call_once(once_, [] {
            boost::log::add_common_attributes();
            boost::log::register_simple_formatter_factory<clio::Severity, char>(
                "Severity");
        });

        namespace src = boost::log::sources;
        namespace keywords = boost::log::keywords;
        namespace sinks = boost::log::sinks;
        namespace expr = boost::log::expressions;
        auto core = boost::log::core::get();

        core->remove_all_sinks();
        boost::log::add_console_log(
            stream_, keywords::format = "%Channel%:%Severity% %Message%");
        auto min_severity = expr::channel_severity_filter(
            clio::log_channel, clio::log_severity);
        min_severity["General"] = clio::Severity::DBG;
        min_severity["Trace"] = clio::Severity::TRC;
        core->set_filter(min_severity);
        core->set_logging_enabled(true);
    }

    void
    checkEqual(std::string expected)
    {
        auto value = buffer_.getStrAndReset();
        ASSERT_EQ(value, expected + '\n');
    }

    void
    checkEmpty()
    {
        ASSERT_TRUE(buffer_.getStrAndReset().empty());
    }
};

/**
 * @brief Fixture with LogService support but completely disabled logging.
 *
 * This is meant to be used as a base for other fixtures.
 */
class NoLoggerFixture : public LoggerFixture
{
protected:
    void
    SetUp() override
    {
        LoggerFixture::SetUp();
        boost::log::core::get()->set_logging_enabled(false);
    }
};

/**
 * @brief Fixture with an embedded boost::asio context running on a thread
 *
 * This is meant to be used as a base for other fixtures.
 */
struct AsyncAsioContextTest : public NoLoggerFixture
{
    AsyncAsioContextTest()
    {
        work.emplace(ctx);  // make sure ctx does not stop on its own
    }

    ~AsyncAsioContextTest()
    {
        work.reset();
        ctx.stop();
        runner.join();
    }

protected:
    boost::asio::io_context ctx;

private:
    std::optional<boost::asio::io_service::work> work;
    std::thread runner{[this] { ctx.run(); }};
};

/**
 * @brief Fixture with an embedded boost::asio context that is not running by
 * default but can be progressed on the calling thread
 *
 * Use `run_for(duration)` etc. directly on `ctx`.
 * This is meant to be used as a base for other fixtures.
 */
struct SyncAsioContextTest : public NoLoggerFixture
{
    SyncAsioContextTest()
    {
    }

protected:
    boost::asio::io_context ctx;
};
