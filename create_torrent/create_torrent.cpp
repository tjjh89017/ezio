#include "libtorrent/entry.hpp"
#include "libtorrent/bencode.hpp"
#include "libtorrent/torrent_info.hpp"
#include "libtorrent/file.hpp"
#include "libtorrent/storage.hpp"
#include "libtorrent/hasher.hpp"
#include "libtorrent/create_torrent.hpp"
#include "libtorrent/file.hpp"
#include "libtorrent/file_pool.hpp"
#include "libtorrent/hex.hpp" 
#include <fstream>
#include <boost/bind.hpp>

#define PATH_MAX 4096

namespace lt = libtorrent;

using namespace std;

bool file_filter(std::string const& f){
	return true;
}

string full_path(char const* path){
	char prefix[PATH_MAX];
	return path[0] == '/' ? string(path) : string(getcwd(prefix,PATH_MAX))+"/"+string(path);
}

string trim_path(string path){
	string trimmed =  path.substr(0,path.substr(0,path.length()-1).find_last_of("/"));
	return trimmed == "" ? "/" : trimmed;
}

void progress_bar(int i, int num){
	cerr << "\r" << i << " " << num << flush;
}

int main(int argc, char const* argv[])
{
	string tracker_uri;
	string source_path;
	string output_path;
	vector<char> torrent;
	lt::file_storage fs;
	fstream torrent_file;
	lt::error_code ec;
	const int flags = lt::create_torrent::optimize_alignment;
	for( int i = 1 ; i < argc ; i++ ){
		switch(int(argv[i][1])){
			case 't': // tracker uri
			case 'T':
				tracker_uri = string(argv[++i]);
				break;
			case 's': // source image
			case 'S':
				source_path = full_path(argv[++i]);
				break;
			case 'o': // output torrent
			case 'O':
				output_path = full_path(argv[++i]);
				break;
			default:
				continue;
				break;
		}
	}
	cerr << "tracker uri: " <<  tracker_uri << endl;
	cerr << "source path: " <<  source_path << endl;
	cerr << "output path: " <<  output_path << endl;
	cerr << "trimmed source path: " <<  trim_path(source_path) << endl;
	try{
		lt::add_files(fs, source_path, file_filter, flags);
		if(fs.num_files() == 0)
		{
			fputs("no files specified.\n", stderr);
			return 1;
		}
		lt::create_torrent t(fs, 0, -1, flags);
		t.add_tracker(tracker_uri);
		lt::set_piece_hashes(t,trim_path(source_path),
			boost::bind(&progress_bar, _1, t.num_pieces()), ec);
		cerr << endl;
		if ( ec ){
			cerr << ec.message() << endl;
			return 1;
		}
		t.set_creator("EZIO@NCTU");
		lt::bencode(back_inserter(torrent), t.generate());
		torrent_file.open( output_path, fstream::out);
		torrent_file << &torrent[0];
	}
	catch (std::exception& e)
	{
		fprintf(stderr, "%s\n", e.what());
	}
}
