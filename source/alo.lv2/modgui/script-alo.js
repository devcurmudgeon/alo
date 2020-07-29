function (event) {

    function handle_event (symbol, value) {
		switch (symbol) {
			case 'level':
				event.icon.find ('[mod-role=level]').text (value.toFixed(2));
			default:
				break;
		}
	}

	if (event.type == 'change') {
		handle_event (event.symbol, event.value);
	}
}