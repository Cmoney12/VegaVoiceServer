#include <iostream>

#include <cstdlib>
#include <deque>
#include <list>
#include <memory>
#include <set>
#include <utility>
#include <boost/asio.hpp>
#include <boost/lexical_cast.hpp>
#include <unordered_map>
#include "Serialization.h"

using boost::asio::ip::tcp;

//----------------------------------------------------------------------

typedef std::deque<std::shared_ptr<Serialization>> chat_message_queue;

//----------------------------------------------------------------------

class chat_participant
{
public:
    virtual ~chat_participant() = default;
    virtual void deliver(const std::string& recipient, const std::shared_ptr<Serialization>& msg) = 0;
};

typedef std::shared_ptr<chat_participant> chat_participant_ptr;

//----------------------------------------------------------------------

class chat_room {
public:

    void join(const std::string& username, const chat_participant_ptr& participant)
    {
        participants_.emplace(username, participant);
        //for (const auto& msg: recent_msgs_)
        //    participant->deliver(msg);
    }

    void leave(const chat_participant_ptr& participant)
    {
        for (auto it = participants_.begin(); it != participants_.end(); )
        {
            if (it->second == participant) {
                participants_.erase(it++);
            }
            else {
                ++it;
            }
        }

    }

    void deliver(const std::string& recipient, const std::shared_ptr<Serialization>& msg)
    {
        if (participants_.find(recipient) != participants_.end()) {
            recent_msgs_.push_back(msg);
            while (recent_msgs_.size() > max_recent_msgs)
                recent_msgs_.pop_front();

            auto it = participants_.find(recipient);
            it->second->deliver(recipient, msg);
        }
    }

private:
    std::unordered_map<std::string, chat_participant_ptr> participants_;
    enum { max_recent_msgs = 100 };
    chat_message_queue recent_msgs_;
};

//----------------------------------------------------------------------

class chat_session
        : public chat_participant,
          public std::enable_shared_from_this<chat_session>
{
public:
    chat_session(tcp::socket socket, chat_room& room, boost::asio::io_context::strand& strand)
            : socket_(std::move(socket)),
              room_(room),
              strand_(strand)
    {
    }

    void start()
    {
        read_phone_number();
        do_read_header();

    }

    void deliver(const std::string& recipient, const std::shared_ptr<Serialization>& msg) override
    {
        bool write_in_progress = !write_msgs_.empty();
        write_msgs_.push_back(msg);
        if (!write_in_progress)
        {
            do_write();
        }
    }

    void read_phone_number() {
        boost::asio::async_read_until(socket_, buf, "\n",
                                      boost::asio::bind_executor(strand_,[this]
                (boost::system::error_code ec, std::size_t size) {
            if(!ec) {
                handle_username(ec, size);
            }
        }));
    }

    void handle_username(boost::system::error_code, std::size_t size) {
        std::stringstream message;

        message << std::istream(&buf).rdbuf();
        buf.consume(size);
        std::string username = message.str();
        message.clear();
        int pos = username.find('\n');
        username = username.substr(0,pos);
        room_.join(username, shared_from_this());
    }

private:
    void do_read_header()
    {
        auto self(shared_from_this());
        boost::asio::async_read(socket_,
                                boost::asio::buffer(read_msg_->head(), Serialization::HEADER_LENGTH),
                                boost::asio::bind_executor(strand_,[this, self](boost::system::error_code ec, std::size_t /*length*/)
                                {
                                    if (!ec && read_msg_->decode_header())
                                    {
                                        do_read_body();
                                    }
                                    else
                                    {
                                        room_.leave(shared_from_this());
                                    }
                                }));
    }

    void do_read_body()
    {
        auto self(shared_from_this());
        boost::asio::async_read(socket_,
                                boost::asio::buffer(read_msg_->body(), read_msg_->body_length()),
                                boost::asio::bind_executor(strand_,[this, self](boost::system::error_code ec, std::size_t /*length*/)
                                {
                                    if (!ec) {
                                        // allow through network to get external ip
                                        std::string client_ip = socket_.remote_endpoint().address().to_string();
                                        std::string s = boost::lexical_cast<std::string>(socket_.remote_endpoint());
                                        std::cout << s << std::endl;
                                        //local ip
                                        //std::string client_ip = socket_.local_endpoint().address().to_string();
                                        std::string username = read_msg_->parse_bson(read_msg_->body(),
                                                                                       read_msg_->body_length(), client_ip);
                                        room_.deliver(username, read_msg_);
                                        do_read_header();
                                    }
                                    else
                                    {
                                        room_.leave(shared_from_this());
                                    }
                                }));
    }

    void do_write()
    {
        auto self(shared_from_this());
        boost::asio::async_write(socket_,
                                 boost::asio::buffer(write_msgs_.front()->data(),
                                                     write_msgs_.front()->length()),
                                 boost::asio::bind_executor(strand_,
                                                            [this, self](boost::system::error_code ec, std::size_t /*length*/)
                                 {
                                     if (!ec)
                                     {
                                         write_msgs_.pop_front();
                                         if (!write_msgs_.empty())
                                         {
                                             do_write();

                                         }
                                     }
                                     else
                                     {
                                         room_.leave(shared_from_this());
                                     }
                                 }));
    }

    tcp::socket socket_;
    chat_room& room_;
    std::shared_ptr<Serialization> read_msg_ = std::make_shared<Serialization>();
    chat_message_queue write_msgs_;
    boost::asio::streambuf buf;
    boost::asio::io_context::strand& strand_;
};

//----------------------------------------------------------------------

class chat_server
{
public:
    chat_server(boost::asio::io_context& io_context,
                const tcp::endpoint& endpoint, boost::asio::io_context::strand& strand)
            : acceptor_(io_context, endpoint), strand_(strand)
    {
        do_accept();
    }

private:
    void do_accept()
    {
        acceptor_.async_accept(
                boost::asio::bind_executor(strand_, [this](boost::system::error_code ec, tcp::socket socket)
                {
                    if (!ec)
                    {
                        std::make_shared<chat_session>(std::move(socket), room_, strand_)->start();
                    }

                    do_accept();
                }));
    }

    tcp::acceptor acceptor_;
    chat_room room_;
    boost::asio::io_context::strand& strand_;
};

//----------------------------------------------------------------------

int main(int argc, char* argv[])
{
    try
    {
        if (argc < 2)
        {
            std::cerr << "Usage: chat_server <port> [<port> ...]\n";
            return 1;
        }
        int port = 1234;
        int thread_number = 3;
        boost::asio::io_context io_context;
        std::vector<std::thread> server_threads;
        tcp::endpoint endpoint(tcp::v4(), port);
        boost::asio::io_context::strand strand_ = boost::asio::io_service::strand(io_context);
        std::shared_ptr<chat_server> server = std::make_shared<chat_server>(io_context, endpoint, strand_);

        // Run the I/O service on the requested number of threads
        boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait(
                [&io_context](boost::system::error_code const&, int)
                {
                    // Stop the io_context. This will cause run()
                    // to return immediately, eventually destroying the
                    // io_context and any remaining handlers in it.
                    io_context.stop();
                });

        server_threads.reserve(thread_number);
        for(auto i = thread_number - 1; i > 0; --i)
            server_threads.emplace_back(
                    [&io_context]
                    {
                        io_context.run();
                    });
        io_context.run();
        for (auto& i: server_threads)
            i.join();
    }
    catch (std::exception& e)
    {
        std::cerr << "Exception: " << e.what() << "\n";
    }

    return 0;
}