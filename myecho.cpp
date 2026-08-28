#include <iostream>

int main(int argc, char *argv[])
{

	for(int arg=0; arg<argc; ++arg)
	{
		if (arg != 0)
		{
			std::cout << " ";
		}
		std::cout << argv[arg];
	}
	return 0;
}
