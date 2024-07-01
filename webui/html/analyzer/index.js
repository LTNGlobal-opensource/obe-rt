$(document).ready(function()
{
 var count = 60;
 var br_data = {
 labels : [
	"","","","","","","","","","",
	"","","","","","","","","","",
	"","","","","","","","","","",
	"","","","","","","","","","",
	"","","","","","","","","","",
	"","","","","","","","","","",
 ],
 datasets : [
 {
   label: "Muxrate",
   fillColor : "rgba(0,220,0,0.5)",
   strokeColor : "rgba(0,220,0,1)",
   pointColor : "rgba(0,220,0,1)",
   pointStrokeColor : "#0ff",
   pointRadius: 1,
   data : [
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     ]
 } ]
};

 var cccount = 60;

 var cc_data = {
 labels : [
	"","","","","","","","","","",
	"","","","","","","","","","",
	"","","","","","","","","","",
	"","","","","","","","","","",
	"","","","","","","","","","",
	"","","","","","","","","","",
 ],
 datasets : [
 {
   label: "Entire Mux",
   fillColor : "rgba(0,220,0,0.5)",
   strokeColor : "rgba(0,220,0,1)",
   pointColor : "rgba(0,220,0,1)",
   pointStrokeColor : "#0ff",
   data : [
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     ]
 } ]
};

 var iatcount = 60;

 var iat_data = {
 labels : [
	"","","","","","","","","","",
	"","","","","","","","","","",
	"","","","","","","","","","",
	"","","","","","","","","","",
	"","","","","","","","","","",
	"","","","","","","","","","",
 ],
 datasets : [
 {
   label: "Network Interface",
   fillColor : "rgba(0,220,0,0.5)",
   strokeColor : "rgba(0,220,0,1)",
   pointColor : "rgba(0,220,0,1)",
   pointStrokeColor : "#0ff",
   data : [
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     ]
 } ]
};

var lastCCValue = 0;
var updateCCData = function(oldVal, newVal, timestr)
{
  if (lastCCValue == newVal) {
    newVal = 0;
  } else {
    if (lastCCValue > 0) {
      var x = newVal;
      newVal = newVal - lastCCValue;
      lastCCValue = x;
    } else {
      lastCCValue = newVal;
    }
  }


  var labels = oldVal["labels"];
  var dataSetInitial = oldVal["datasets"][0]["data"];
  labels.shift();
  cccount++;
  labels.push(timestr);
  var newData = Math.floor(newVal);
  dataSetInitial.push(newData);
  dataSetInitial.shift();
};

var updateIATData = function(oldVal, newVal, timestr, nicname)
{
  var labels = oldVal["labels"];
  var dataset = oldVal["datasets"];
  oldVal.datasets.label = nicname + ' Interface';
  var dataSetInitial = oldVal["datasets"][0]["data"];
  labels.shift();
  iatcount++;
  labels.push(timestr);
  var newData = Math.floor(newVal);
  dataSetInitial.push(newData);
  dataSetInitial.shift();
};

var updateBitrateData = function(oldVal, newVal, timestr)
{
  var labels = oldVal["labels"];
  var dataSetInitial = oldVal["datasets"][0]["data"];
  labels.shift();
  count++;
  labels.push(timestr);
  var newData = Math.floor(newVal);
  dataSetInitial.push(newData);
  dataSetInitial.shift();
};


var ctx = document.getElementById("myChart").getContext("2d");
var br_chart = new Chart(ctx,
{
	type: 'line', data: br_data, options:
	{
		animation: false,
		scales: {
			yAxes: [{ display: true, ticks:
				{ beginAtZero: true, steps: 10, stepValue: 5, max: 40 }
				}]
		},
		title: {
     display: true,
      text: 'Bitrate (Mb/ps)'
    }
	}
});

//var cc_chartctx = document.getElementById("ccChart").getContext("2d");
//var cc_chart = new Chart(cc_chartctx,
//{
//	type: 'bar', data: cc_data, options:
//	{
//		animation: false,
//		scales: {
//			yAxes: [{ display: true, ticks:
//				{ beginAtZero: true, steps: 1, stepValue: 2, max: 10 }
//				}]
//		},
//		title: {
//                   display: true,
//                    text: 'Continuity Errors'
//                }
//	}
//});

//var iat_chartctx = document.getElementById("iatChart").getContext("2d");
//var iat_chart = new Chart(iat_chartctx,
//{
//	type: 'bar', data: iat_data, options:
//	{
//		animation: false,
//		scales: {
//			yAxes: [
//        {
//          display: true, ticks: { beginAtZero: true, steps: 1, stepValue: 2, max: 10 }
//				}
//      ],
//			xAxes: [
//        {
//          ticks: {
//            stepSize: 1,
//            unitStepSize: 1
//          },
//        }
//      ],
//    },
//		title: {
//      display: true,
//      text: 'Maximum Inter-packet Arrival Times (IAT)'
//    }
//	}
//});

function sortTable() {
  var table, rows, switching, i, x, y, shouldSwitch;
  table = document.getElementById("pidtable");
  switching = true;
  /* Make a loop that will continue until
  no switching has been done: */
  while (switching) {
    // Start by saying: no switching is done:
    switching = false;
    rows = table.rows;
    /* Loop through all table rows (except the
    first, which contains table headers): */
    for (i = 1; i < (rows.length - 1); i++) {
      // Start by saying there should be no switching:
      shouldSwitch = false;
      /* Get the two elements you want to compare,
      one from current row and one from the next: */
      x = rows[i].getElementsByTagName("TD")[0];
      y = rows[i + 1].getElementsByTagName("TD")[0];
      // Check if the two rows should switch place:
      if (x.innerHTML.toLowerCase() > y.innerHTML.toLowerCase()) {
        // If so, mark as a switch and break the loop:
        shouldSwitch = true;
        break;
      }
    }
    if (shouldSwitch) {
      /* If a switch has been marked, make the switch
      and mark that a switch has been done: */
      rows[i].parentNode.insertBefore(rows[i + 1], rows[i]);
      switching = true;
    }
  }
}

function iso8601_to_str(d)
{
  return d.substring(0,10) + ' ' + d.substring(11,19);
}

function updatePidTableServiceType(pid, desc)
{
    var table = document.getElementById("pidtable");
    for (var i = 0, row; row = table.rows[i]; i++) {
      if (row.cells[0].innerHTML === pid) {
          if (desc == "PCR" && row.cells[4].innerHTML != "" && row.cells[4].innerHTML.includes("PCR") == 0) {
            row.cells[4].innerHTML = row.cells[4].innerHTML + ' (PCR)';
          } else {
            row.cells[4].innerHTML = desc;
          }
          break;
      } 
   }

};

function updatePidTable(pid, packetcount, mbps, cc)
{
    var table = document.getElementById("pidtable");
    var found = 0;
    for (var i = 1, row; row = table.rows[i]; i++) {
      if (row.cells[0].innerHTML === pid) {
          row.cells[1].innerHTML = packetcount;
          row.cells[2].innerHTML = mbps;
          row.cells[3].innerHTML = cc;
          found = 1;
          break;
      } 
   }

   if (found == 0) {
      var row = table.insertRow(-1);
      row.insertCell(0);
      row.insertCell(1);
      row.insertCell(2);
      row.insertCell(3);
      row.insertCell(4);
      row.cells[0].innerHTML = pid;
      row.cells[1].innerHTML = packetcount;
      row.cells[2].innerHTML = mbps;
      row.cells[3].innerHTML = cc;
      if (pid == "0x0000") {
        row.cells[4].innerHTML = "PAT";
      } else
      if (pid == "0x1fff") {
        row.cells[4].innerHTML = "Null Padding";
      } else {
        row.cells[4].innerHTML = "?";
      }
      sortTable();
    }

    return 0;
};

function processAudio(nr, level, inputtype)
{
  if (parseInt(level) != 0) {
    document.getElementById('a_ch' + nr).style.width = (90 + level) * 3.5;
  } else {
    document.getElementById('a_ch' + nr).style.width = 0;
    if (inputtype != 'bitstream') {
      document.getElementById('a_ch' + nr).style.border = 'none';
    }
  }

  if (inputtype == "pcm") {
    document.getElementById('a_ch' + nr).innerHTML = 'PCM ' + level;
  }
  if (inputtype == "bitstream") {
    document.getElementById('a_ch' + nr).style.width = 100;
    document.getElementById('a_ch' + nr).innerHTML = 'Bitstream';
  }
  if (inputtype == "NA") {
    document.getElementById('a_ch' + nr).innerHTML = '-';
  }
}

function webSocketInvoke()
{
  if ("WebSocket" in window) {
    console.log("WebSocket is supported by your Browser!");
    var ws = new WebSocket("ws://192.168.2.45:13400/stream/live?01","echo-protocol");
    ws.onopen = function() {
      console.log("Connection created");
      ws.binaryType = 'arraybuffer';
     };
 
    ws.onmessage = function (evt) {

      var enc = new TextDecoder("utf-8");
      var jdata = enc.decode(evt.data);

      console.log(jdata);

      var blob = JSON.parse(jdata);  
      //var blob = received_msg.text();  
      //var blob = JSON.stringify(received_msg.text());


      document.getElementById('uuid').innerHTML = blob.uuid;
      document.getElementById('starttime').innerHTML = iso8601_to_str(blob.startup);
      document.getElementById('runtime').innerHTML = blob.runtime;
      document.getElementById('loadavg').innerHTML = blob.platform.la1 + ', ' + blob.platform.la5 + ', ' + blob.platform.la15;
      document.getElementById('hostname').innerHTML = blob.platform.host;
      document.getElementById('processid').innerHTML = blob.platform.pid;
      document.getElementById('cpupct').innerHTML = blob.platform.cpu_pct + '%';
      document.getElementById('temperature').innerHTML = blob.platform.temperature + ' degC';
      document.getElementById('signalformat').innerHTML = blob.signal.format;
      document.getElementById('los_time').innerHTML = iso8601_to_str(blob.signal.los_time);
      document.getElementById('acq_time').innerHTML = iso8601_to_str(blob.signal.acq_time);
      document.getElementById('pkg_version').innerHTML = blob.platform.pkg_version;

      document.getElementById('model').innerHTML = blob.platform.model + ' / ' +
       blob.signal.type + ' / ' + blob.signal.inputport;

      document.getElementById('cfgtime').innerHTML = blob.platform.last_conf_file_change;

      document.getElementById('lastsmpte2038').innerHTML = iso8601_to_str(blob.signal.smpte2038_time);
      document.getElementById('lastscte104').innerHTML = iso8601_to_str(blob.signal.scte104_time);
      document.getElementById('cea708_time').innerHTML = iso8601_to_str(blob.signal.cea708_time);
      document.getElementById('dstname').innerHTML = blob.output.url;
      document.getElementById('scte35_count').innerHTML = blob.output.scte35_count;

      // KB to MB
      var hdfree_root = parseInt(blob.platform.hdfree_root) / 1000000000;
      var hdfree_root = hdfree_root.toFixed(2);
      var hdfree_storage = parseInt(blob.platform.hdfree_storage) / 1000000000;
      var hdfree_storage = hdfree_storage.toFixed(2);

      document.getElementById('hdfreespace').innerHTML = hdfree_root + ' GB / ' + hdfree_storage + ' GB';
      
      // KB to MB
      var memused = parseInt(blob.platform.memused) / 1000;
      document.getElementById('memused').innerHTML = memused.toFixed(2) + ' MB';

      var br = parseInt(blob.output.br) / 1000000;
      document.getElementById('bitrate').innerHTML = br.toFixed(2) + ' Mb';

      document.getElementById('tnImage').src = blob.signal.thumbnail + '?t=' + new Date().getTime();

      // Audio
      processAudio(1, blob.a1_level, blob.a1_type);
      processAudio(2, blob.a2_level, blob.a2_type);
      processAudio(3, blob.a3_level, blob.a3_type);
      processAudio(4, blob.a4_level, blob.a4_type);
      processAudio(5, blob.a5_level, blob.a5_type);
      processAudio(6, blob.a6_level, blob.a6_type);
      processAudio(7, blob.a7_level, blob.a7_type);
      processAudio(8, blob.a8_level, blob.a8_type);
      processAudio(9, blob.a9_level, blob.a9_type);
      processAudio(10, blob.a10_level, blob.a10_type);
      processAudio(11, blob.a11_level, blob.a11_type);
      processAudio(12, blob.a12_level, blob.a12_type);
      processAudio(13, blob.a13_level, blob.a13_type);
      processAudio(14, blob.a14_level, blob.a14_type);
      processAudio(15, blob.a15_level, blob.a15_type);
      processAudio(16, blob.a16_level, blob.a16_type);
      
      updateBitrateData(br_data, br, blob.now.substring(11, 19));
      //updateCCData(cc_data, blob.stats.ccerrors, blob.timestamp.substring(11, 19));
      //updateIATData(iat_data, blob.stats.iat1_max, blob.timestamp.substring(11, 19), blob.stats.nic);

      //document.getElementById('srcname').innerHTML = blob.src;
      //document.getElementById('dstname').innerHTML = blob.dst;
      //document.getElementById('ccerrors').innerHTML = blob.stats.ccerrors;
      //document.getElementById('packetcount').innerHTML = blob.stats.packetcount;
      //document.getElementById('nicname').innerHTML = blob.stats.nic;

      // Update the pid table with basic stats
      //blob.output.pids.forEach((item) => {
      //  updatePidTable(item.nr, item.cc, item.cc, item.type);
      //});

      // Update the pid table for each service (video, audio, etc)
      //blob.services.forEach((svc) => {
      //  updatePidTableServiceType(svc.pmtpid, "PMT");
      //  svc.streams.forEach((strm) => {
      //    updatePidTableServiceType(strm.pid, strm.desc);
      //  });
      //  updatePidTableServiceType(svc.pcrpid, "PCR");
      //});

      br_chart.update();
      //cc_chart.update();
      //iat_chart.update();
      //console.log(received_msg);
    };
 
    ws.onclose = function() { 
      console.log("Connection closed"); 
    };

 } else {
   alert("WebSocket NOT supported by your Browser!");
 }

}

webSocketInvoke();

});

