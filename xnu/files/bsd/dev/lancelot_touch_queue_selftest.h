/* Included after touch_post(); only before device creation / producer startup.
 * The kernel runs this only with touch_selftest=1. Host tests compile this same
 * code and the actual queue functions. No synthetic sample escapes the test. */
static void
lt_queue_reset(void)
{
	touch_head = touch_tail = 0;
	for (unsigned i = 0; i < TOUCH_QUEUE; i++) touch_queue[i] = (struct redwood_touch){0};
	for (unsigned i = 0; i < TOUCH_CONTACTS; i++) {
		queued_contact[i] = false; queued_x[i] = queued_y[i] = 0;
	}
	for (unsigned i = 0; i <= REDWOOD_BUTTON_HOME; i++) queued_button[i] = false;
}

static bool
lt_queue_drain(bool *down, bool *buttons)
{
	if (touch_tail - touch_head > TOUCH_QUEUE) return false;
	while (touch_head != touch_tail) {
		struct redwood_touch t = touch_queue[touch_head++ % TOUCH_QUEUE];
		if (t.type == REDWOOD_BUTTON_DOWN || t.type == REDWOOD_BUTTON_UP) {
			if (t.finger < 1 || t.finger > REDWOOD_BUTTON_HOME) return false;
			buttons[t.finger] = t.type == REDWOOD_BUTTON_DOWN;
			continue;
		}
		if (t.finger >= TOUCH_CONTACTS || t.x > 65535 || t.y > 65535) return false;
		if (t.type == REDWOOD_TOUCH_DOWN) down[t.finger] = true;
		else if (t.type == REDWOOD_TOUCH_MOVE) { if (!down[t.finger]) return false; }
		else if (t.type == REDWOOD_TOUCH_UP) down[t.finger] = false;
		else return false;
	}
	return true;
}

static unsigned
lancelot_touch_queue_selftest(void)
{
	bool down[TOUCH_CONTACTS] = {0}, buttons[REDWOOD_BUTTON_HOME + 1] = {0};
	unsigned failed = 0;
	uint32_t saved_lat = touch_lat;
#define LTQ_CHECK(test) do { if (!(test)) { failed = __LINE__; goto done; } } while (0)
	touch_lat = 0;
	lt_queue_reset();
	for (unsigned i = 0; i < TOUCH_CONTACTS; i++) touch_post(REDWOOD_TOUCH_DOWN, i, 100 + i, 200 + i, false);
	LTQ_CHECK(lt_queue_drain(down, buttons));
	/* This UP is discarded at overflow although HID still holds finger 9. */
	touch_post(REDWOOD_TOUCH_UP, 9, 109, 209, false);
	for (unsigned i = 0; i < TOUCH_QUEUE - 1; i++) touch_post(REDWOOD_TOUCH_MOVE, 0, i, i, false);
	LTQ_CHECK(touch_tail - touch_head == TOUCH_QUEUE);
	touch_post(REDWOOD_TOUCH_MOVE, 0, 1000, 2000, false);
	LTQ_CHECK(lt_queue_drain(down, buttons) && !down[9]);
	for (unsigned i = 0; i < 9; i++) LTQ_CHECK(down[i]);
	/* 511 queued records plus an orphan MOVE requires two free records. */
	for (unsigned i = 0; i < TOUCH_QUEUE - 1; i++) touch_post(REDWOOD_TOUCH_MOVE, 0, i, i, false);
	touch_post(REDWOOD_TOUCH_MOVE, 9, 70000, 80000, false);
	LTQ_CHECK(touch_tail - touch_head <= TOUCH_QUEUE);
	LTQ_CHECK(lt_queue_drain(down, buttons) && down[9]);
	LTQ_CHECK(queued_x[9] == 65535 && queued_y[9] == 65535);
	/* A pending button UP cannot be lost just because touches fill the queue. */
	touch_post(REDWOOD_BUTTON_DOWN, REDWOOD_BUTTON_LOCK, 0, 0, false);
	LTQ_CHECK(lt_queue_drain(down, buttons) && buttons[REDWOOD_BUTTON_LOCK]);
	touch_post(REDWOOD_BUTTON_UP, REDWOOD_BUTTON_LOCK, 0, 0, false);
	for (unsigned i = 0; i < TOUCH_QUEUE; i++) touch_post(REDWOOD_TOUCH_MOVE, 0, i, i, false);
	LTQ_CHECK(lt_queue_drain(down, buttons) && !buttons[REDWOOD_BUTTON_LOCK]);
	/* Unsigned head/tail arithmetic must survive counter wrap. */
	touch_head = touch_tail = UINT32_MAX - 5u;
	for (unsigned i = 0; i < TOUCH_CONTACTS; i++) touch_post(REDWOOD_TOUCH_UP, i, 0, 0, false);
	LTQ_CHECK(lt_queue_drain(down, buttons));
	for (unsigned i = 0; i < TOUCH_CONTACTS; i++) LTQ_CHECK(!down[i]);
done:
	lt_queue_reset();
	touch_lat = saved_lat;
	return failed;
#undef LTQ_CHECK
}
