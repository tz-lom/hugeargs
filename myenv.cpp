#include <iostream>

extern char **environ;

int main()
{
	for (char **env = environ; *env != nullptr; ++env)
	{
		std::cout << *env << '\n';
	}
	return 0;
}
