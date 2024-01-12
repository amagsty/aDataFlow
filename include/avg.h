#ifndef _AVG_H
#define _AVG_H

class movAvg3
{
private:
    uint32_t arr[3];
    uint8_t index;

public:
    void push(uint32_t value)
    {
        arr[index] = value;
        index = index < 3 ? index + 1 : 0;
    }
    uint32_t result()
    {
        return (arr[0] + arr[1] + arr[2]) / 3;
    }
};


#endif