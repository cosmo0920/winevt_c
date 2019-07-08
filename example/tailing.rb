require 'winevt'
require 'rexml/document'

@subscribe = Winevt::EventLog::Subscribe.new
@subscribe.tail = true
@subscribe.subscribe("Security", "*[System[(Level <= 4) and TimeCreated[timediff(@SystemTime) <= 86400000]]]")
while (1) do
  if @subscribe.next
    eventlog = @subscribe.render
    message = @subscribe.message
    string_inserts = @subscribe.string_inserts
    message = message.gsub(/(%\d+)/, '\1$s')
    message = sprintf(message, *string_inserts)

    puts ({eventlog: eventlog, data: message})
    @subscribe.close_handle # Dispose EVT_HANDLE variable which is allocated in EvtNext
  else
    printf(".")
    sleep(1)
  end
end
puts @subscribe.bookmark
