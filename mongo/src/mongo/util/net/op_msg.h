/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/net/message.h"

namespace mongo {

//mongodb报文body解析封装:mongodb>=3.6，都是OP_MSG,OpMsg针对opCode=OP_MSG，mongodb<3.6，opCode=[dbUpdate, dbDelete]，对应body解析由DbMessage
//内容来源见OpMsg::parse   
struct OpMsg { //下面的OpMsgRequest继承该类  例如insert操作对应的OpMsg使用见Insert::parseProtected
    struct DocumentSequence {
        std::string name;
        std::vector<BSONObj> objs;
    };

    //是否需要校验
    static constexpr uint32_t kChecksumPresent = 1 << 0;
    //不应答客户端，见runCommands  Strategy::clientCommand
    static constexpr uint32_t kMoreToCome = 1 << 1;

    /**
     * Returns the unvalidated flags for the given message if it is an OP_MSG message.
     * Returns 0 for other message kinds since they are the equivalent of no flags set.
     * Throws if the message is too small to hold flags.
     */
    static uint32_t flags(const Message& message);
    static bool isFlagSet(const Message& message, uint32_t flag) {
        return flags(message) & flag;
    }

    /**
     * Replaces the flags in message with the supplied flags.
     * Only legal on an otherwise valid OP_MSG message.
     */
    static void replaceFlags(Message* message, uint32_t flags);

    /**
     * Adds flag to the list of set flags in message.
     * Only legal on an otherwise valid OP_MSG message.
     */
    static void setFlag(Message* message, uint32_t flag) {
        replaceFlags(message, flags(*message) | flag);
    }

    /**
     * Parses and returns an OpMsg containing unowned BSON.
     */
    static OpMsg parse(const Message& message);

    /**
     * Parses and returns an OpMsg containing owned BSON.
     */
    static OpMsg parseOwned(const Message& message) {
        auto msg = parse(message);
        msg.shareOwnershipWith(message.sharedBuffer());
        return msg;
    }

    Message serialize() const;

    /**
     * Makes all BSONObjs in this object share ownership with buffer.
     */
    void shareOwnershipWith(const ConstSharedBuffer& buffer);

    /**
     * Returns a pointer to the sequence with the given name or nullptr if there are none.
     */
    const DocumentSequence* getSequence(StringData name) const {
        // Getting N sequences is technically O(N**2) but because there currently is at most 2
        // sequences, this does either 1 or 2 comparisons. Consider making sequences a StringMap if
        // there will be many sequences. This problem may also just go away with the IDL project.
        auto it = std::find_if(
            sequences.begin(), sequences.end(), [&](const auto& seq) { return seq.name == name; });
        return it == sequences.end() ? nullptr : &*it;
    }

    //msg解析赋值见OpMsg::parse   body的第一个elem也就是命令名，如find insert等
    BSONObj body; //赋值见OpMsg::parse   输出打印通过:c->getRedactedCopyForLogging(request.body);
    //sequences用法暂时没看懂，感觉没什么用？先跳过
    std::vector<DocumentSequence> sequences; //赋值见OpMsg::parse
};

/**
 * An OpMsg that represents a request. This is a separate type from OpMsg only to provide better
 * type-safety along with a place to hang request-specific methods.
 */
//协议解析得时候会用到，见runCommands，例如insert操作对应的OpMsg使用见Insert::parseProtected
struct OpMsgRequest : public OpMsg {
    // TODO in C++17 remove constructors so we can use aggregate initialization.
    OpMsgRequest() = default;
    //构造初始化
    explicit OpMsgRequest(OpMsg&& generic) : OpMsg(std::move(generic)) {}

    //opMsgRequestFromAnyProtocol->OpMsgRequest::parse 
    //从message中解析出OpMsg所需成员信息
    static OpMsgRequest parse(const Message& message) {
        //OpMsg::parse
        return OpMsgRequest(OpMsg::parse(message));
    }

    //根据db body extraFields填充OpMsgRequest
    static OpMsgRequest fromDBAndBody(StringData db,
                                      BSONObj body,
                                      const BSONObj& extraFields = {}) {
        OpMsgRequest request;
        request.body = ([&] {
            BSONObjBuilder bodyBuilder(std::move(body));
            bodyBuilder.appendElements(extraFields);
            bodyBuilder.append("$db", db);
            return bodyBuilder.obj();
        }());
        return request;
    }

    //从body中获取db name
    StringData getDatabase() const {
        if (auto elem = body["$db"])
            return elem.checkAndGetStringData();
        uasserted(40571, "OP_MSG requests require a $db argument");
    }

    //find  insert 等  body中的第一个elem就是command 名
    StringData getCommandName() const {
        return body.firstElementFieldName();
    }

    // DO NOT ADD MEMBERS!  Since this type is essentially a strong typedef (see the class comment),
    // it should not hold more data than an OpMsg. It should be freely interconvertible with OpMsg
    // without issues like slicing.
};

/**
 * Builds an OP_MSG message in-place in a Message buffer.
 *
 * While the OP_MSG format imposes no ordering of sections, in order to efficiently support our
 * usage patterns, this class requires that all document sequences (if any) are built before the
 * body. This allows repeatedly appending fields to the body until right before it is ready to be
 * sent.
 */
class OpMsgBuilder {
    MONGO_DISALLOW_COPYING(OpMsgBuilder);

public:
    OpMsgBuilder() {
        skipHeaderAndFlags();
    }

    /**
     * See the documentation for DocSequenceBuilder below.
     */
    class DocSequenceBuilder;
    DocSequenceBuilder beginDocSequence(StringData name);

    /**
     * Returns an empty builder for the body.
     * It is an error to call this if a body has already been begun.  You must destroy or call
     * done() on the returned builder before calling any methods on this object.
     */
    BSONObjBuilder beginBody();
    void setBody(const BSONObj& body) {
        beginBody().appendElements(body);
    }

    /**
     * Returns a builder that can be used to append new fields to the body.
     * It is an error to call this if beginBody() hasn't been called yet. It is an error to append
     * elements with field names that already exist in the body. You must destroy or call done() on
     * the returned builder before calling any methods on this object.
     *
     * TODO decide if it is worth keeping the begin/resume distinction in the public API.
     */
    BSONObjBuilder resumeBody();
    void appendElementsToBody(const BSONObj& body) {
        resumeBody().appendElements(body);
    }

    /**
     * Finish building and return a Message ready to give to the networking layer for transmission.
     * It is illegal to call any methods on this object after calling this.
     */
    Message finish();

    /**
     * Reset this object to its initial empty state. All previously appended data is lost.
     */
    void reset() {
        invariant(!_openBuilder);

        _buf.reset();
        skipHeaderAndFlags();
        _bodyStart = 0;
        _state = kEmpty;
        _openBuilder = false;
    }

    /**
     * Set to true in tests that need to be able to generate duplicate top-level fields to see how
     * the server handles them. Is false by default, although the check only happens in debug
     * builds.
     */
    static AtomicBool disableDupeFieldCheck_forTest;

private:
    friend class DocSequenceBuilder;

    //赋值见OpMsg::parse，对应msg解析的几个阶段
    enum State {
        kEmpty,
        kDocSequence,
        kBody,
        kDone,
    };

    void finishDocumentStream(DocSequenceBuilder* docSequenceBuilder);

    void skipHeaderAndFlags() {
        _buf.skip(sizeof(MSGHEADER::Layout));  // This is filled in by finish().
        _buf.appendNum(uint32_t(0));           // flags (currently always 0).
    }

    // When adding members, remember to update reset().
    BufBuilder _buf;
    int _bodyStart = 0;
    State _state = kEmpty;
    bool _openBuilder = false;
};

/**
 * Builds a document sequence in an OpMsgBuilder.
 *
 * Example:
 *
 * auto docSeq = msgBuilder.beginDocSequence("some.sequence");
 *
 * docSeq.append(BSON("a" << 1)); // Copy an obj into the sequence
 *
 * auto bob = docSeq.appendBuilder(); // Build an obj in-place
 * bob.append("a", 2);
 * bob.doneFast();
 *
 * docSeq.done(); // Or just let it go out of scope.
 */
class OpMsgBuilder::DocSequenceBuilder {
    MONGO_DISALLOW_COPYING(DocSequenceBuilder);

public:
    DocSequenceBuilder(DocSequenceBuilder&& other)
        : _buf(other._buf), _msgBuilder(other._msgBuilder), _sizeOffset(other._sizeOffset) {
        other._buf = nullptr;
    }

    ~DocSequenceBuilder() {
        if (_buf)
            done();
    }

    /**
     * Indicates that the caller is done with this stream prior to destruction.
     * Following this call, it is illegal to call any methods on this object.
     */
    void done() {
        invariant(_buf);
        _msgBuilder->finishDocumentStream(this);
        _buf = nullptr;
    }

    /**
     * Appends a single document to this sequence.
     */
    void append(const BSONObj& obj) {
        _buf->appendBuf(obj.objdata(), obj.objsize());
    }

    /**
     * Returns a BSONObjBuilder that appends a single document to this sequence in place.
     * It is illegal to call any methods on this DocSequenceBuilder until the returned builder
     * is destroyed or done()/doneFast() is called on it.
     */
    BSONObjBuilder appendBuilder() {
        return BSONObjBuilder(*_buf);
    }

private:
    friend OpMsgBuilder;

    DocSequenceBuilder(OpMsgBuilder* msgBuilder, BufBuilder* buf, int sizeOffset)
        : _buf(buf), _msgBuilder(msgBuilder), _sizeOffset(sizeOffset) {}

    BufBuilder* _buf;
    OpMsgBuilder* const _msgBuilder;
    const int _sizeOffset;
};

}  // namespace mongo
