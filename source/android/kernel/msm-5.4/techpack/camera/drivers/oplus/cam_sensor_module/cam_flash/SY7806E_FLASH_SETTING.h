{
	.need_standby_mode = 0,
	.flashprobeinfo =
	{
		.flash_name = "sy7806e",
		.slave_write_address = 0xc6,
		.flash_id_address = 0x0C,
		.flash_id = 0x1C,
		.addr_type = CAMERA_SENSOR_I2C_TYPE_BYTE,
		.data_type = CAMERA_SENSOR_I2C_TYPE_BYTE,
	},
    .cci_client =
	{
        .cci_i2c_master = MASTER_0,
        .i2c_freq_mode = I2C_STANDARD_MODE,
        .sid = 0xc6 >> 1,
	},
	.flashinitsettings =
	{
		.reg_setting =
		{
			{.reg_addr = 0x01, .reg_data = 0x80, .delay = 0x00, .data_mask = 0x00}, \
			{.reg_addr = 0x03, .reg_data = 0x5d, .delay = 0x00, .data_mask = 0x00}, \
			{.reg_addr = 0x05, .reg_data = 0x23, .delay = 0x00, .data_mask = 0x00}, \
			{.reg_addr = 0x08, .reg_data = 0x1f, .delay = 0x00, .data_mask = 0x00}, \
		},
		.size = 4,
		.addr_type = CAMERA_SENSOR_I2C_TYPE_BYTE,
		.data_type = CAMERA_SENSOR_I2C_TYPE_BYTE,
		.delay = 1,
	},
	.flashhighsettings =
	{
		.reg_setting =
		{
			{.reg_addr = 0x01, .reg_data = 0x8d, .delay = 0x00, .data_mask = 0x00}, \
		},
		.size = 1,
		.addr_type = CAMERA_SENSOR_I2C_TYPE_BYTE,
		.data_type = CAMERA_SENSOR_I2C_TYPE_BYTE,
		.delay = 1,
	},
	.flashlowsettings =
	{
		.reg_setting =
		{
			{.reg_addr = 0x01, .reg_data = 0x89, .delay = 0x00, .data_mask = 0x00}, \
		},
		.size = 1,
		.addr_type = CAMERA_SENSOR_I2C_TYPE_BYTE,
		.data_type = CAMERA_SENSOR_I2C_TYPE_BYTE,
		.delay = 1,
	},
	.flashoffsettings =
	{
		.reg_setting =
		{
			{.reg_addr = 0x01, .reg_data = 0x80, .delay = 0x00, .data_mask = 0x00}, \
		},
		.size = 1,
		.addr_type = CAMERA_SENSOR_I2C_TYPE_BYTE,
		.data_type = CAMERA_SENSOR_I2C_TYPE_BYTE,
		.delay = 1,
	},
	.flashpowerupsetting =
	{
		.seq_type = SENSOR_CUSTOM_GPIO1,
		.config_val = 1,
		.delay = 5,
		.size = 1,
	},
	.flashpowerdownsetting =
	{
		.seq_type = SENSOR_CUSTOM_GPIO1,
		.config_val = 0,
		.delay = 1,
		.size = 1,
	},
},