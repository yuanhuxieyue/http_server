#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>
#include <fmt/format.h>
#include <string.h>
#include <cstddef>
#include <thread>

// 处理异常小工具
int check_error(const char* msg, int res) {
	if (res == -1) {
		fmt::print("{}:{}\n", msg, strerror(errno));
		throw;
	}
	return res;
}

// read返回的是size_t类型
size_t check_error(const char* msg, ssize_t res) {
	if (res == -1) {
		fmt::print("{}:{}\n", msg, strerror(errno));
		throw;
	}
	return res;
}

#define CHECK_CALL(func, ...) check_error(#func, func(__VA_ARGS__))

// 存储地址及长度
struct socket_address_fatptr {
	struct sockaddr* m_addr;
	socklen_t m_addrlen;
};

//存放连接的客户端信息
struct socket_address_storage {
	union {
		struct sockaddr m_addr;
		struct sockaddr_storage m_addr_storage;
	};
	socklen_t m_addrlen = sizeof(struct socket_address_storage);

	operator socket_address_fatptr() {
		return { &m_addr, m_addrlen };
	}
};

//处理地址表项
struct address_resolved_entry {
	struct addrinfo* m_curr = nullptr;

	socket_address_fatptr get_address() const {
		return { m_curr->ai_addr, m_curr->ai_addrlen };
	}

	int create_socket() const {
		int sockfd = CHECK_CALL(socket, m_curr->ai_family, m_curr->ai_socktype, m_curr->ai_protocol);
		return sockfd;
	}

	//封装创建绑定套件字过程
	int create_socket_and_bind() const {
		int sockfd = create_socket();
		socket_address_fatptr server_addr = get_address();
		CHECK_CALL(bind, sockfd, server_addr.m_addr, server_addr.m_addrlen);
		return sockfd;
	}

	[[nodiscard]] bool next_entry() {
		m_curr = m_curr->ai_next;
		if (m_curr == nullptr) {
			return false;
		}
		return true;
	}
};

//读地址表
struct address_resolver {
	struct addrinfo* m_head = nullptr;

	void resolve(std::string const& name, std::string const& service) {
		int err = getaddrinfo(name.c_str(), service.c_str(), NULL, &m_head);
		if (err != 0) {
			fmt::print("getaddrinfo:{} {}\n", gai_strerror(err), err);
			throw;
		}
	}

	address_resolved_entry get_first_entry() {
		return { m_head };
	}

	address_resolver() = default;

	address_resolver(address_resolver&& that) : m_head(that.m_head) {
		that.m_head = nullptr;
	}

	~address_resolver() {
		if (m_head) {
			freeaddrinfo(m_head);
		}
	}
};

using StringMap = std::unordered_map<std::string, std::string>;

//解析http1.1
struct http11_header_parser {
	std::string m_header;		//"GET / ... \r\nConnection: close"
	std::string m_header_line;	//"GET / HTTP/1.1" 第一行
	StringMap m_header_keys;	//{"HOST": "tsy", "Connection: close"}
	std::string m_body;			//超量读取的正文

	bool m_header_finished = false;

	[[nodiscard]] bool header_finished() const {
		return m_header_finished;
	}

	//提取头部信息
	void _extract_headers() {
		size_t pos = m_header.find("\r\n");
		while (pos != std::string::npos) {
			pos += 2; //跳过\r\n
			//通过寻找下一个键值信息来切下本行
			size_t next_pos = m_header.find("\r\n", pos);
			size_t line_len = std::string::npos;
			if (next_pos != std::string::npos) {
				line_len = next_pos - pos;
			}
			std::string line = m_header.substr(pos, line_len);
			size_t colon = line.find(": ");
			if (colon != std::string::npos) {
				std::string key = line.substr(0, colon);
				std::string value = line.substr(colon + 2);
				//因大小写不敏感而全部转为小写
				std::transform(key.begin(), key.end(), key.begin(), [](char c) {
					if ('A' <= c && c <= 'Z') {
						c += 'a' - 'A';
					}
					return c;
				});
				// C++17新写法，较m_header_keys[key] = value;高效
				m_header_keys.insert_or_assign(std::move(key), value);
			}
			pos = next_pos;
		}
	}

	void push_chunk(std::string_view chunk) {
		if (!m_header_finished) {
			m_header.append(chunk);
			size_t header_len = m_header.find("\r\n\r\n");
			if (header_len != std::string::npos) {
				// 头部读取结束
				m_header_finished = true;
				m_body = m_header.substr(header_len + 4); //留下超量读取的正文
				m_header.resize(header_len); //？头部过长出错处理
				_extract_headers(); //开始解析头部
			}
		} 
	}

	std::string& headline() {
		return m_header_line;
	}

	std::string& headers_raw() {
		return m_header;
	}

	StringMap& headers() {
		return m_header_keys;
	}

	std::string& extra_body() {
		return m_body;
	}
};

template <class HeaderParser = http11_header_parser>
struct http_request_parser {
	HeaderParser m_header_parser;
	size_t m_content_length = 0;
	bool m_body_finished = false;

	[[nodiscard]] bool request_finished() {
		return m_body_finished;
	}

	std::string& body() {
		return m_header_parser.extra_body();
	}

	std::string& headers_raw() {
		return m_header_parser.headers_raw();
	}

	StringMap& headers() {
		return m_header_parser.headers();
	}

	std::string method() {
		auto& headline = m_header_parser.headline();
		size_t space = headline.find(' ');
		if (space != std::string::npos) {
			return headline.substr(0, space);
		} else {
			return "GET";
		}
	}

	std::string url() {
		auto& headline = m_header_parser.headline();
		size_t space1 = headline.find(' ');
		if (space1 == std::string::npos) {
			return "GET";
		}
		size_t space2 = headline.find(' ', space1);
		if (space2 == std::string::npos) {
			return "GET";
		}
		return headline.substr(space1, space2);
	}

	size_t _extract_content_length() {
		auto& headers = m_header_parser.headers();
		auto it = headers.find("content-length");
		if (it == headers.end()) {
			return 0;
		}
		try {
			std::stoi(it->second);
		}
		catch (std::invalid_argument const &) {
			return 0;
		}
		
	}

	void push_chunk(std::string_view chunk) {
		if (!m_header_parser.header_finished()) {
			m_header_parser.push_chunk(chunk);
			if (m_header_parser.header_finished()) {
				m_content_length = _extract_content_length();
				if (body().size() >= m_content_length) {
					m_body_finished = true;
					body().resize(m_content_length);
				}
			}
		}
	}

};

std::vector<std::thread> pool;

int main() {
	setlocale(LC_ALL, "zh_CN.UTF-8");
	address_resolver resolver;
	resolver.resolve("192.168.10.110", "6688");
	fmt::print("正在监听192.168.10.110:6688\n");
	auto entry = resolver.get_first_entry();
	int listenfd = entry.create_socket_and_bind();
	CHECK_CALL(listen, listenfd, SOMAXCONN);

	while (true) {
		socket_address_storage addr;
		int connid = CHECK_CALL(accept, listenfd, &addr.m_addr, &addr.m_addrlen);
		pool.emplace_back([connid] {
			char buf[1024];
			http_request_parser req_parse;
			do {
				size_t n = CHECK_CALL(read, connid, buf, sizeof(buf));
				req_parse.push_chunk(std::string_view(buf, n));
			} while (!req_parse.request_finished());
			fmt::print("收到请求头:\n{}\n", req_parse.headers_raw());
			fmt::print("收到请求体:\n{}\n", req_parse.body());
			std::string body = req_parse.body();

			std::string res = "HTTP/1.1 200 OK\r\nServer: my_server\r\nConnection: close\r\nContent-length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
			fmt::print("反馈:\n{}\n", res);
			CHECK_CALL(write, connid, res.data(), res.size());
			close(connid);
			});
	}
	for (auto& t : pool) {
		t.join();
	}
	return 0;
}