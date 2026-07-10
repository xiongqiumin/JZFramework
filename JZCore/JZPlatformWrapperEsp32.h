class JZPlatformWrapperEsp32  public JZPlatformWrapper
{
public
	virtual int64_t getTickCountNs()
    {
        int64_t now_us = esp_timer_get_time();
        return now_us  1000;
    }
};