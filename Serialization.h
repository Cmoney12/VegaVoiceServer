//
// Created by corey on 9/5/21.
//

#ifndef SIP_SERVER_SERIALIZATION_H
#define SIP_SERVER_SERIALIZATION_H

#include <bson.h>

class Serialization {
public:
    enum { HEADER_LENGTH = 4 };

    Serialization():body_length_(0){}

    uint8_t* data() const
    {
        return data_.get();
    }

    const uint8_t* data()
    {
        return data_.get();
    }

    uint8_t* head() {
        return header;
    }

    std::size_t length() const
    {
        return HEADER_LENGTH + body_length_;
    }

    uint8_t* body()
    {
        return data_.get() + HEADER_LENGTH;
    }

    int body_length() const
    {
        return body_length_;
    }

    void set_size(const std::size_t& size) {
        body_length_ = (int)size;
        data_ = std::make_unique<uint8_t[]>(size + HEADER_LENGTH);
    }

    const char* parse_bson(const uint8_t *bson_data, std::size_t size, const std::string& ip_address) {

        bson_t *received;
        bson_iter_t iter;
        uint32_t size1;
        const char *receiver = nullptr;

        received = bson_new_from_data(bson_data, size);

        if (bson_iter_init_find(&iter, received, "Receivers_Number") && BSON_ITER_HOLDS_UTF8(&iter)) {
            receiver = bson_iter_utf8(&iter, nullptr);
        }

        // If we receive an accept we append the ip address of the sender to data
        // portion of the packet
        if (bson_iter_init_find(&iter, received, "Status_Code") && BSON_ITER_HOLDS_INT32(&iter)) {
            int32_t status_code = bson_iter_int32(&iter);
            if (status_code == 200 || status_code == 100) {

                data_.release();
                bson_append_utf8(received, "Data", -1, ip_address.c_str(), -1);
                body_length_ = (int)received->len;
                const uint8_t *bson = bson_destroy_with_steal(received, true, &size1);
                data_ = std::make_unique<uint8_t[]>(body_length_ + HEADER_LENGTH);
                encode_header();
                std::memcpy(data_.get() + HEADER_LENGTH, bson, body_length_);
                return receiver;

            }
        }

        bson_destroy(received);

        return receiver;
    }

    bool decode_header() {
        std::memcpy(&body_length_, header, sizeof body_length_);
        set_size(body_length_);
        //std::memcpy(data_, header, HEADER_LENGTH);
        data_.get()[3] = (body_length_>>24) & 0xFF;
        data_.get()[2] = (body_length_>>16) & 0xFF;
        data_.get()[1] = (body_length_>>8) & 0xFF;
        data_.get()[0] = body_length_ & 0xFF;
        if(body_length_ > MAX_MESSAGE_SIZE) {
            body_length_ = 0;
            return false;
        }
        return true;
    }

    bool encode_header() const {
        if (body_length_ <= MAX_MESSAGE_SIZE && body_length_) {
            data_[3] = (body_length_>>24) & 0xFF;
            data_[2] = (body_length_>>16) & 0xFF;
            data_[1] = (body_length_>>8) & 0xFF;
            data_[0] = body_length_ & 0xFF;
            return true;
        }
        return false;
    }


private:
    int body_length_{};
    std::unique_ptr<uint8_t[]> data_{};
    enum { MAX_MESSAGE_SIZE = 999999 };
    uint8_t header[HEADER_LENGTH]{};

private:

};

#endif //SIP_SERVER_SERIALIZATION_H
