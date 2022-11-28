// declarations
var filename = null;
var fileData = null;
var fileStg = null;
var once = false;
var host = location.host;
var espUrl = "http://" + host;
var e;
function readSingleFile(f) {
    e = f;
    document.getElementById("up").addEventListener("click", UploadFile, false); // to add onclick() to Upload button
}
//append table rows
function GenerateRows(filename, filesize) {
    var table = document.getElementById("table").getElementsByTagName('tbody')[0];
    if (filename != "" && filesize != undefined) {
        var count = table.rows.length;
        var row = table.insertRow(count);
        var cell1 = row.insertCell(0);
        var cell2 = row.insertCell(1);
        cell1.innerHTML = filename;
        cell2.innerHTML = filesize;
    }
}
function DisplayDirectory(requestList) {
    once = true;
    var table = document.getElementById("table").getElementsByTagName('tbody')[0];
    var listData = requestList.split("\n"); // decode
    if (listData.length == 0) {
        alert("No data");
    } for (var x = 0; x < listData.length; x++) {
        // if (listData[x].length < 5) {
        //     continue;
        // }
        // parse
        var elements = listData[x].split(";");
        console.log("feild1 : " + elements[0]);
        console.log("feild2 :" + elements[1]);
        if (elements[0] != "" && elements[1] != undefined) {
            if (once) {
                while (table.rows.length > 1) {
                    table.deleteRow(1);
                }
                once = false;
            }
        }
        GenerateRows(elements[0], elements[1]);
    }
}
// current files in the file system
function fetchFileList() {
    var xhr = new XMLHttpRequest();
    var url = espUrl + "/pgm?cmd=list"; xhr.open("GET", url);
    xhr.setRequestHeader("Content-type", "application/x-www-form-urlencoded");
    xhr.onreadystatechange = function () {
        console.log("status : " + xhr.status);
        console.log("respons :" + xhr.responseText); if (xhr.readyState == 4 && xhr.status == 200) //done & success
        {
            DisplayDirectory(xhr.responseText);
        }
    }
    xhr.send();
}

function UploadData() {
    var file = e.target.files[0]; //read first incoming file
    var filename = file.name;
    if (!file) {
        return;
    }
    let formData = new FormData();
    formData.append("file", file, filename); var xhr = new XMLHttpRequest();
    var url = espUrl + "/pgm"; xhr.open("POST", url, true);
    xhr.send(formData);
    e = null;
}

function UploadFile() {
    UploadData();
    setTimeout(fetchFileList, 2000);
}

function deleteFile(filename) {
    var xhr = new XMLHttpRequest();
    var url = espUrl + "/pgm?cmd=del&filename=" + filename; xhr.open("GET", url, true);
    xhr.setRequestHeader("Content-type", "application/x-www-form-urlencoded");
    xhr.onreadystatechange = function () {
        if (xhr.readyState == 4 && xhr.status == 200) {
            DisplayDirectory(xhr.responseText);
        }
    }
    xhr.send();
}

function flashFile(filename) {
    document.getElementById('status').innerHTML = "Flashing in progress.";
    var xhr = new XMLHttpRequest();
    var url = espUrl + "/pgm?cmd=flash&filename=" + filename; xhr.open("GET", url, true);
    xhr.setRequestHeader("Content-type", "application/x-www-form-urlencoded");
    xhr.onreadystatechange = function () {
    if (xhr.readyState == 4 && xhr.status == 200) {
    alert(xhr.responseText);
    }
    }
    xhr.send();

    var statusfunc = setInterval(function () {
    var xhttp = new XMLHttpRequest();
    xhttp.onreadystatechange = function () {
    if (this.readyState == 4 && this.status == 200) {
    data = this.responseText;
    document.getElementById('status').innerHTML = data;
    clearInterval(statusfunc);
    }
    };
    xhttp.open('GET', '/pgmstatus');
    xhttp.send();
    }, 2000);
    }

/*   Highlight selected row containing file to be staged  */
$('table#table.styled-table').on('click', 'tr', function (event) {
    var $this = jQuery(this);
    fileStg = $(this).find('td:first').text();
    if (fileStg != "" && fileStg != "File") {
        console.log(fileStg + " is staged to be flashed/deleted");


        $(this).css('background-color', 'green');
        jQuery(".highlight").removeClass("highlight").css('background-color', '');

        $this.addClass("highlight");
    }
});


// global listeners
document.getElementById("flash").addEventListener("click", function () {
    flashFile(fileStg);
});
document.getElementById("delete").addEventListener("click", function () {
    deleteFile(fileStg);
});
document.getElementById('file-7').addEventListener('change', readSingleFile, false); window.onload = function () {
    fetchFileList();
};