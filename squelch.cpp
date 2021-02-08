#include "squelch.h"

#include "rtl_airband.h" // needed for debug_print()

using namespace std;

Squelch::Squelch(int manual) :
	manual_(manual)
{
	noise_floor_ = 100.0f;
	pre_filter_avg_ = 0.5f;
	post_filter_avg_ = 0.5f;

	using_post_filter_ = false;

	// TODO: Possible Improvement - revisit magic numbers
	open_delay_ = 197;
	close_delay_ = 197;
	low_power_abort_ = 88;

	next_state_ = CLOSED;
	current_state_ = CLOSED;

	delay_ = 0;
	open_count_ = 0;
	sample_count_ = 0;
	low_power_count_ = 0;

	debug_print("Created Squelch, open_delay_: %d, close_delay_: %d, low_power_abort: %d, manual: %d\n", open_delay_, close_delay_, low_power_abort_, manual_);
}

bool Squelch::is_open(void) const {
	return (current_state_ == OPEN);
}

bool Squelch::should_filter_sample(void) const {
	return (current_state_ == OPEN || current_state_ == OPENING);
}

bool Squelch::first_open_sample(void) const {
	return (next_state_ == OPENING && current_state_ != OPENING);
}

bool Squelch::last_open_sample(void) const {
	return (next_state_ == CLOSING && current_state_ != CLOSING);
}

const Squelch::State & Squelch::get_state(void) const {
	return current_state_;
}

const float & Squelch::noise_floor(void) const {
	return noise_floor_;
}

const float & Squelch::power_level(void) const {
	return pre_filter_avg_;
}

const size_t & Squelch::open_count(void) const {
	return open_count_;
}

float Squelch::squelch_level(void) const {
	if (is_manual()) {
		return manual_;
	}
	return 3.0f * noise_floor();
}

bool Squelch::is_manual(void) const {
	return manual_ >= 0;
}

bool Squelch::has_power(void) const {
	if (using_post_filter_) {
		return power_level() >= squelch_level() && post_filter_avg_ >= pre_filter_avg_;
	}
	return power_level() >= squelch_level();
}

void Squelch::process_reference_sample(const float &sample) {

	// Update current state based on previous state from last iteration
	update_current_state();

	sample_count_++;

	// auto noise floor
	// TODO: Possible Improvement - update noise floor with each sample
	// TODO: what is the purpose of the adding 0.0001f every loop?  This could account for squelch flap on marginal signal
	if (sample_count_ % 16 == 0) {
		noise_floor_ = noise_floor_ * 0.97f + std::min(pre_filter_avg_, noise_floor_) * 0.03f + 0.0001f;
	}

	// average power
	pre_filter_avg_ = pre_filter_avg_ * 0.99f + sample * 0.01f;

	// Check power against thresholds
	if (current_state_ == OPEN && has_power() == false) {
		debug_print("Closing at %zu: no power after timeout (%f < %f)\n", sample_count_, power_level(), squelch_level());
		set_state(CLOSING);
	}

	if (current_state_ == CLOSED && has_power() == true) {
		debug_print("Opening at %zu: power (%f >= %f)\n", sample_count_, power_level(), squelch_level());
		set_state(OPENING);
	}

	// Override squelch and close if there are repeated samples under the squelch level
	// NOTE: this can cause squelch to close, but it may immediately be re-opened if the power level still hasn't fallen after the delays
	if((current_state_ == OPEN || current_state_ == OPENING) && next_state_ != CLOSING) {
		if (sample >= squelch_level()) {
			low_power_count_ = 0;
		} else {
			low_power_count_++;
			if (low_power_count_ >= low_power_abort_) {
				debug_print("Closing at %zu: low power count %d\n", sample_count_, low_power_count_);
				set_state(CLOSING);
			}
		}
	}
}

void Squelch::process_filtered_sample(const float &sample) {
	if (should_filter_sample() == false) {
		return;
	}

	// average power
	using_post_filter_ = true;
	post_filter_avg_ = post_filter_avg_ * 0.999f + sample * 0.001f;

	if ((current_state_ == OPEN || current_state_ == OPENING || next_state_ == OPEN || next_state_ == OPENING) && post_filter_avg_ < pre_filter_avg_) {
		debug_print("Closing at %zu: power post filter (%f < %f)\n", sample_count_, post_filter_avg_, squelch_level());
		set_state(CLOSING);
	}
}

void Squelch::set_state(State update) {

	// Valid transitions (current_state_ -> next_state_) are:
	//  - OPENING -> OPENING
	//  - OPENING -> CLOSING
	//  - OPENING -> OPEN
	//  - OPEN -> OPEN
	//  - OPEN -> CLOSING
	//  - CLOSING -> OPENING
	//  - CLOSING -> CLOSING
	//  - CLOSING -> CLOSED
	//  - CLOSED -> CLOSED
	//  - CLOSED -> OPENING

	// Invalid transistions (current_state_ -> next_state_) are:
	//  - OPENING -> CLOSED (must go through CLOSING to get to CLOSED)
	//  - OPEN -> OPENING (if already OPEN cant go backwards)
	//  - OPEN -> CLOSED (must go through CLOSING to get to CLOSED)
	//  - CLOSING -> OPEN (must go through OPENING to get to OPEN)
	//  - CLOSED -> CLOSING (if already CLOSED cant go backwards)
	//  - CLOSED -> OPEN (must go through OPENING to get to OPEN)

	// must go through OPENING to get to OPEN (unless already OPEN)
	if (update == OPEN && current_state_ != OPEN && current_state_ != OPENING) {
		update = OPENING;
	}

	// must go through CLOSING to get to CLOSED (unless already CLOSED)
	if (update == CLOSED && current_state_ != CLOSING && current_state_ != CLOSED) {
		update = CLOSING;
	}

	// if already OPEN cant go backwards
	if (update == OPENING && current_state_ == OPEN) {
		update = OPEN;
	}

	// if already CLOSED cant go backwards
	if (update == CLOSING && current_state_ == CLOSED) {
		update = CLOSED;
	}

	next_state_ = update;
}

void Squelch::update_current_state(void) {
	if (next_state_ == OPENING) {
		if (current_state_ != OPENING) {
			debug_print("%zu: transitioning to OPENING\n", sample_count_);
			open_count_++;
			delay_ = open_delay_;
			low_power_count_ = 0;
			post_filter_avg_ = pre_filter_avg_;
			current_state_ = next_state_;
		} else {
			// in OPENING delay
			delay_--;
			if (delay_ <= 0) {
				next_state_ = OPEN;
			}
		}
	} else if (next_state_ == CLOSING) {
		if (current_state_ != CLOSING) {
			debug_print("%zu: transitioning to CLOSING\n", sample_count_);
			delay_ = close_delay_;
			current_state_ = next_state_;
		} else {
			// in CLOSING delay
			delay_--;
			if (delay_ <= 0) {
				next_state_ = CLOSED;
			}
		}
	} else if (next_state_ == OPEN && current_state_ != OPEN) {
		debug_print("%zu: transitioning to OPEN\n", sample_count_);
		current_state_ = next_state_;
	} else if (next_state_ == CLOSED && current_state_ != CLOSED) {
		debug_print("%zu: transitioning to CLOSED\n", sample_count_);
		using_post_filter_ = false;
		current_state_ = next_state_;
	} else {
		current_state_ = next_state_;
	}
}