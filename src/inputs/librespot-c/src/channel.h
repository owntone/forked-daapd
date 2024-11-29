struct sp_channel *
channel_get(uint32_t channel_id, struct sp_session *session);

void
channel_free(struct sp_channel *channel);

void
channel_free_all(struct sp_session *session);

int
channel_new(struct sp_channel **channel, struct sp_session *session, const char *path, struct event_base *evbase, event_callback_fn write_cb);

int
channel_data_write(struct sp_channel *channel);

void
channel_play(struct sp_channel *channel);

void
channel_stop(struct sp_channel *channel);

int
channel_seek(struct sp_channel *channel, size_t pos);

void
channel_pause(struct sp_channel *channel);

void
channel_retry(struct sp_channel *channel);

int
channel_msg_read(uint16_t *channel_id, uint8_t *msg, size_t msg_len, struct sp_session *session);

int
channel_http_body_read(struct sp_channel *channel, uint8_t *body, size_t body_len);
