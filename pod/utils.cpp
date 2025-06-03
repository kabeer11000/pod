#include <iostream>
#include <vector>
#include <string>

void print_vector(std::ostream &os, const std::vector<std::string> &vec)
{
    // use iterating syntax because you dont care about the index
    for (const auto value : vec)
    {
        os << "[";
        for (size_t i = 0; i < vec.size(); ++i)
        {
            std::cout << vec[i];
            if (i < vec.size() - 1)
            {
                std::cout << ", ";
            }
        }
        os << "]" << std::endl;
    }
}