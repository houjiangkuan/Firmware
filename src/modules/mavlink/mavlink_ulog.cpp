/****************************************************************************
 *
 *   Copyright (c) 2016 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file mavlink_ulog.cpp
 * ULog data streaming via MAVLink
 *
 * @author Beat Küng <beat-kueng@gmx.net>
 */

#include "mavlink_ulog.h"
#include <px4_log.h>
#include <errno.h>

bool MavlinkULog::_init = false;
MavlinkULog *MavlinkULog::_instance = nullptr;
sem_t MavlinkULog::_lock;


MavlinkULog::MavlinkULog()
{
	_ulog_stream_sub = orb_subscribe(ORB_ID(ulog_stream));

	if (_ulog_stream_sub < 0) {
		PX4_ERR("orb_subscribe failed (%i)", errno);
	}
	_waiting_for_initial_ack = true;
	_last_sent_time = hrt_absolute_time(); //(ab)use this timestamp during initialization
}

MavlinkULog::~MavlinkULog()
{
	if (_ulog_stream_ack_pub) {
		orb_unadvertise(_ulog_stream_ack_pub);
	}
	if (_ulog_stream_sub >= 0) {
		orb_unsubscribe(_ulog_stream_sub);
	}
}

void MavlinkULog::start_ack_received()
{
	if (_waiting_for_initial_ack) {
		_last_sent_time = 0;
		_waiting_for_initial_ack = false;
		PX4_DEBUG("got logger ack");
	}
}

int MavlinkULog::handle_update(mavlink_channel_t channel)
{
	static_assert(sizeof(ulog_stream_s::data) == MAVLINK_MSG_LOGGING_DATA_FIELD_DATA_LEN, "Invalid uorb ulog_stream.data length");
	static_assert(sizeof(ulog_stream_s::data) == MAVLINK_MSG_LOGGING_DATA_ACKED_FIELD_DATA_LEN, "Invalid uorb ulog_stream.data length");

	if (_waiting_for_initial_ack) {
		if (hrt_elapsed_time(&_last_sent_time) > 3e5) {
			PX4_WARN("no ack from logger (is it running?)");
			return -1;
		}
	}

	// check if we're waiting for an ACK
	if (_last_sent_time) {
		bool check_for_updates = false;
		if (_ack_received) {
			_last_sent_time = 0;
			check_for_updates = true;
		} else {

			if (hrt_elapsed_time(&_last_sent_time) > ulog_stream_ack_s::ACK_TIMEOUT * 1000) {
				if (++_sent_tries > ulog_stream_ack_s::ACK_MAX_TRIES) {
					return -ETIMEDOUT;
				} else {
					PX4_DEBUG("re-sending ulog mavlink message (try=%i)", _sent_tries);
					_last_sent_time = hrt_absolute_time();
					mavlink_logging_data_acked_t msg;
					msg.sequence = _ulog_data.sequence;
					msg.length = _ulog_data.length;
					msg.first_message_offset = _ulog_data.first_message_offset;
					memcpy(msg.data, _ulog_data.data, sizeof(msg.data));
					mavlink_msg_logging_data_acked_send_struct(channel, &msg);
				}
			}
		}

		if (!check_for_updates) {
			return 0;
		}
	}

	bool updated = false;
	int ret = orb_check(_ulog_stream_sub, &updated);
	while (updated && !ret) {
		orb_copy(ORB_ID(ulog_stream), _ulog_stream_sub, &_ulog_data);
		if (_ulog_data.flags & ulog_stream_s::FLAGS_NEED_ACK) {
			_sent_tries = 1;
			_last_sent_time = hrt_absolute_time();
			lock();
			_wait_for_ack_sequence = _ulog_data.sequence;
			_ack_received = false;
			unlock();

			mavlink_logging_data_acked_t msg;
			msg.sequence = _ulog_data.sequence;
			msg.length = _ulog_data.length;
			msg.first_message_offset = _ulog_data.first_message_offset;
			memcpy(msg.data, _ulog_data.data, sizeof(msg.data));
			mavlink_msg_logging_data_acked_send_struct(channel, &msg);

		} else {
			mavlink_logging_data_t msg;
			msg.sequence = _ulog_data.sequence;
			msg.length = _ulog_data.length;
			msg.first_message_offset = _ulog_data.first_message_offset;
			memcpy(msg.data, _ulog_data.data, sizeof(msg.data));
			mavlink_msg_logging_data_send_struct(channel, &msg);
		}
		ret = orb_check(_ulog_stream_sub, &updated);
	}

	return 0;
}

void MavlinkULog::initialize()
{
	if (_init) {
		return;
	}
	sem_init(&_lock, 1, 1);
	_init = true;
}

MavlinkULog* MavlinkULog::try_start()
{
	MavlinkULog *ret = nullptr;
	bool failed = false;
	lock();
	if (!_instance) {
		ret = _instance = new MavlinkULog();
		if (!_instance) {
			failed = true;
		}
	}
	unlock();

	if (failed) {
		PX4_ERR("alloc failed");
	}
	return ret;
}

void MavlinkULog::stop()
{
	lock();
	if (_instance) {
		delete _instance;
		_instance = nullptr;
	}
	unlock();
}

void MavlinkULog::handle_ack(mavlink_logging_ack_t ack)
{
	lock();
	if (_instance) { // make sure stop() was not called right before
		if (_wait_for_ack_sequence == ack.sequence) {
			_ack_received = true;
			publish_ack(ack.sequence);
		}
	}
	unlock();
}

void MavlinkULog::publish_ack(uint16_t sequence)
{
	ulog_stream_ack_s ack;
	ack.timestamp = hrt_absolute_time();
	ack.sequence = sequence;

	if (_ulog_stream_ack_pub == nullptr) {
		_ulog_stream_ack_pub = orb_advertise_queue(ORB_ID(ulog_stream_ack), &ack, 3);

	} else {
		orb_publish(ORB_ID(ulog_stream_ack), _ulog_stream_ack_pub, &ack);
	}
}
