/*
 * jquery.xslt.js
 *
 * Copyright (c) 2005-2008 Johann Burkard (<mailto:jb@eaio.com>)
 * <http://eaio.com>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 * NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 */
 
/**
 * jQuery client-side XSLT plugins.
 * 
 * @author <a href="mailto:jb@eaio.com">Johann Burkard</a>
 * @version $Id: jquery.xslt.js,v 1.10 2008/08/29 21:34:24 Johann Exp $
 */
(function($) {
    $.fn.xslt = function() {
        return this;
    }
    var str = /^\s*</;
    if ($.browser.msie) { // IE 5+
        $.fn.xslt = function(xml, xslt, callback) {
            var target = $(this);

            var xmlResult = $.ajax(
            {
              url: xml,
              async: false
            }).responseXML;

            var xsltResult = $.ajax(
            {
              url: xslt,
              async: false
            }).responseXML;
                
            target.html(xmlResult.transformNode(xsltResult));
            if (callback) {
              callback();
            }

            return this;
       };
    }
    else if (window.DOMParser != undefined && window.XMLHttpRequest != undefined && window.XSLTProcessor != undefined) { // Mozilla 0.9.4+, Opera 9+
       var processor = new XSLTProcessor();
       var support = false;
       if ($.isFunction(processor.transformDocument)) {
           support = window.XMLSerializer != undefined;
       }
       else {
           support = true;
       }
       if (support) {
            $.fn.xslt = function(xml, xslt, callback) {
                var target = $(this);
                var transformed = false;

                var xm = {
                    readyState: 4
                };
                var xs = {
                    readyState: 4
                };
                
                var xsDoc = null;
                var xmDoc = null;

                var change = function() {
                    if(xm.readyState == 4){xmDoc = xm.responseXML;}
                    if(xs.readyState == 4){xsDoc = xs.responseXML;}
                    if (xmDoc && xsDoc && !transformed) {
                        var processor = new XSLTProcessor();
                        if ($.isFunction(processor.transformDocument)) {
                            // obsolete Mozilla interface
                            resultDoc = document.implementation.createDocument("", "", null);
                            processor.transformDocument(xmDoc, xsDoc, resultDoc, null);
                            target.html(new XMLSerializer().serializeToString(resultDoc));
                        }
                        else {
                            processor.importStylesheet(xsDoc);
                            resultDoc = processor.transformToFragment(xmDoc, document);
                            target.empty().append(resultDoc);
                        }
                        transformed = true;
                        callback();
                    }
                };

                if (str.test(xml)) {
                    xm.responseXML = new DOMParser().parseFromString(xml, "text/xml");
                }
                else {
                    xm = $.ajax({dataType: "xml", url: xml});
                    xm.onreadystatechange = change;
                }

                if (str.test(xslt)) {
                    xs.responseXML = new DOMParser().parseFromString(xslt, "text/xml");
                    change();
                }
                else {
                    xs = $.ajax({dataType: "xml", url: xslt});
                    xs.onreadystatechange = change;
                }
                return this;
            };
       }
    }
})(jQuery);
