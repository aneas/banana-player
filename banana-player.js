var child_process = require('child_process'),
    http          = require('http'),
    fs            = require('fs'),
    querystring   = require('querystring'),
    socketio      = require('socket.io'),
    express       = require('express');

var mplayer = {
	process: null,
	status: {
		loaded:   false,
		paused:   false,
		position: 0,
		length:   0,
		path:     '',
		filename: ''
	}
};

function createMPlayer() {
	mplayer.process = child_process.spawn('mplayer', ['-slave', '-quiet', '-idle']);

	mplayer.process.stdout.setEncoding('utf8');

	// this is a hack to prevent crashes: https://github.com/joyent/node/issues/6043
	mplayer.process.stdin.on('error', function(e) { console.log(e); });

	mplayer.process.stdout.on('data', function(data) {
		var lines = data.toString().trim().split(/\r?\n/);

		var statusChanged = false;

		lines.forEach(function(data) {
			console.log(data);

			if(data == 'Starting playback...') {
				mplayer.status.loaded = true;
				statusChanged = true;
				refreshStatus();
			}

			if(data.substr(0, 4) == 'ANS_') {
				data = data.substr(4);
				var pos = data.indexOf('=');
				if(pos != -1) {
					var key   = data.substr(0, pos),
					    value = data.substr(pos + 1);
					switch(key) {
						case 'pause':    mplayer.status.paused   = (value != 'no'); statusChanged = true; break;
						case 'time_pos': mplayer.status.position = +value;          statusChanged = true; break;
						case 'length':   mplayer.status.length   = +value;          statusChanged = true; break;
						case 'path':     mplayer.status.path     =  value;          statusChanged = true; break;
						case 'filename': mplayer.status.filename =  value;          statusChanged = true; break;
					}
				}
			}
		});

		if(statusChanged)
			io.emit('status', mplayer.status);
	});

	mplayer.process.on('exit', function() {
		mplayer.process = null;
		mplayer.status = {
			loaded:   false,
			paused:   false,
			position: 0,
			length:   0,
			path:     '',
			filename: ''
		};
		io.emit('status', mplayer.status);
	});
}

function refreshStatus() {
	if(mplayer.process && mplayer.status.loaded) {
		mplayer.process.stdin.write('pausing_keep_force get_property pause\n');
		mplayer.process.stdin.write('pausing_keep_force get_property time_pos\n');
		mplayer.process.stdin.write('pausing_keep_force get_property length\n');
		mplayer.process.stdin.write('pausing_keep_force get_property path\n');
		mplayer.process.stdin.write('pausing_keep_force get_property filename\n');
	}
}

setInterval(function() {
	if(mplayer.process && mplayer.status.loaded) {
		mplayer.process.stdin.write('pausing_keep_force get_property pause\n');
		mplayer.process.stdin.write('pausing_keep_force get_property time_pos\n');
	}
}, 1000);

var app    = express();
var server = http.Server(app);
var io     = socketio(server);

app.use(express.static('public'));

io.on('connection', function(socket) {
	io.emit('status', mplayer.status);

	socket.on('command', function(msg) {
		// console.log(msg);

		switch(msg.type) {
			case 'load':
				if(!mplayer.process)
					createMPlayer();
				mplayer.process.stdin.write('loadfile "' + msg.path + '"\n');
				mplayer.process.stdin.write('vo_fullscreen\n');
				break;

			case 'pause':
				if(mplayer.process) {
					mplayer.process.stdin.write('pause\n');
					refreshStatus();
				}
				break;

			case 'stop':
				if(mplayer.process) {
					mplayer.process.stdin.write('quit\n');
					refreshStatus();
				}
				break;

			case 'fullscreen':
				if(mplayer.process)
					mplayer.process.stdin.write('pausing_keep_force vo_fullscreen 1\n');
				break;

			case 'rewind1m':
				if(mplayer.process) {
					mplayer.process.stdin.write('pausing_keep seek -60 0\n');
					refreshStatus();
				}
				break;

			case 'rewind10s':
				if(mplayer.process) {
					mplayer.process.stdin.write('pausing_keep seek -10 0\n');
					refreshStatus();
				}
				break;

			case 'forward10s':
				if(mplayer.process) {
					mplayer.process.stdin.write('pausing_keep seek 10 0\n');
					refreshStatus();
				}
				break;

			case 'forward1m':
				if(mplayer.process) {
					mplayer.process.stdin.write('pausing_keep seek 60 0\n');
					refreshStatus();
				}
				break;

			case 'seek':
				if(mplayer.process) {
					console.log(msg.position);
					mplayer.process.stdin.write('pausing_keep seek ' + msg.position + ' 1\n');
					refreshStatus();
				}
				break;
		}
	});

	socket.on('browse', function(path, callback) {
		var entries = fs.readdirSync(path);
		var directories = [],
		    files       = [];
		entries.forEach(function(entry) {
			if(entry[0] === '.')
				return;
			var stats = fs.statSync(path + '/' + entry);
			if(stats.isFile())
				files.push({ name: entry, size: stats.size });
			else if(stats.isDirectory())
				directories.push({ name: entry });
		});

		directories.sort(function(a, b) { return a.name.localeCompare(b.name); });
		files.sort(function(a, b) { return a.name.localeCompare(b.name); });
		callback({ directories: directories, files: files });
	});
});

server.listen(3000, function(){
  console.log('listening on *:3000');
});
